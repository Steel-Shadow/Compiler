#include "middle/Optimize.h"

#include "middle/IRUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IR {
namespace {

bool isRecursive(const Function &function) {
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            const auto *callee = inst.op == Op::Call
                                         ? dynamic_cast<const Label *>(inst.arg1.get())
                                         : nullptr;
            if (callee && callee->nameAndId == function.getName()) {
                return true;
            }
        }
    }
    return false;
}

std::vector<size_t> argumentPushes(
        const std::vector<Inst> &instructions,
        size_t call) {
    size_t frameStart = call;
    size_t nesting = 0;
    for (size_t reverse = call; reverse-- > 0;) {
        if (instructions[reverse].op == Op::OutStack) {
            ++nesting;
        } else if (instructions[reverse].op == Op::InStack) {
            if (nesting == 0) {
                frameStart = reverse;
                break;
            }
            --nesting;
        }
    }
    if (frameStart == call) {
        return {};
    }

    std::vector<size_t> pushes;
    nesting = 0;
    for (size_t index = frameStart + 1; index < call; ++index) {
        if (instructions[index].op == Op::InStack) {
            ++nesting;
        } else if (instructions[index].op == Op::OutStack) {
            if (nesting == 0) {
                return {};
            }
            --nesting;
        } else if (nesting == 0
                   && (instructions[index].op == Op::PushParam
                       || instructions[index].op == Op::PushAddressParam)) {
            pushes.push_back(index);
        }
    }
    if (nesting != 0) {
        return {};
    }
    std::reverse(pushes.begin(), pushes.end());
    return pushes;
}

std::string specializationName(
        const std::string &callee,
        const std::vector<std::optional<int>> &arguments) {
    std::string name = callee + "__const";
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index]) {
            continue;
        }
        name += "_" + std::to_string(index) + "_";
        if (*arguments[index] < 0) {
            name += "m" + std::to_string(-static_cast<long long>(*arguments[index]));
        } else {
            name += std::to_string(*arguments[index]);
        }
    }
    return name;
}

std::unique_ptr<Element> cloneElement(
        const Element *element,
        const std::unordered_map<std::string, std::string> &labels) {
    if (!element) {
        return nullptr;
    }
    if (const auto *label = dynamic_cast<const Label *>(element)) {
        auto mapped = labels.find(label->nameAndId);
        return mapped == labels.end()
                       ? label->clone()
                       : std::make_unique<Label>(mapped->second, true);
    }
    return element->clone();
}

std::unique_ptr<Function> cloneSpecializedFunction(
        const Function &source,
        const std::string &name,
        const std::vector<std::optional<int>> &arguments) {
    auto result = std::make_unique<Function>(
            name, source.getReturnType(), source.getParams());
    BasicBlocks blocks;
    std::unordered_map<std::string, std::string> labels;
    for (size_t index = 0; index < source.getBasicBlocks().size(); ++index) {
        const auto &sourceBlock = source.getBasicBlocks()[index];
        auto block = index == 0
                             ? std::make_unique<BasicBlock>(name, true)
                             : std::make_unique<BasicBlock>(name + "_block");
        labels.emplace(sourceBlock->label.nameAndId, block->label.nameAndId);
        blocks.push_back(std::move(block));
    }

    for (size_t block = 0; block < source.getBasicBlocks().size(); ++block) {
        for (const auto &inst: source.getBasicBlocks()[block]->instructions) {
            const auto *formal = inst.op == Op::Parameter
                                         ? dynamic_cast<const Var *>(inst.arg1.get())
                                         : nullptr;
            if (formal) {
                auto parameter = std::find_if(
                        source.getParams().begin(), source.getParams().end(),
                        [&](const ParamInfo &candidate) {
                            return candidate.name == formal->name;
                        });
                if (parameter != source.getParams().end()) {
                    const size_t index = static_cast<size_t>(
                            std::distance(source.getParams().begin(), parameter));
                    if (index < arguments.size() && arguments[index]) {
                        blocks[block]->instructions.emplace_back(
                                Op::LoadImd,
                                inst.res->clone(),
                                std::make_unique<ConstVal>(
                                        *arguments[index], parameter->type),
                                nullptr);
                        continue;
                    }
                }
            }

            Inst clone(inst.op,
                       cloneElement(inst.res.get(), labels),
                       cloneElement(inst.arg1.get(), labels),
                       cloneElement(inst.arg2.get(), labels));
            for (const auto &incoming: inst.phiIncoming) {
                auto predecessor = labels.find(incoming.predecessor);
                clone.addPhiIncoming(
                        predecessor == labels.end()
                                ? incoming.predecessor
                                : predecessor->second,
                        cloneElement(incoming.value.get(), labels));
            }
            blocks[block]->instructions.push_back(std::move(clone));
        }
    }
    result->moveBasicBlocks(std::move(blocks));
    return result;
}

struct SpecializationRequest {
    const Function *source{};
    std::string name;
    std::vector<std::optional<int>> arguments;
};

bool specializeInCaller(
        Function &caller,
        const std::unordered_map<std::string, const Function *> &callees,
        std::unordered_set<std::string> &existing,
        std::vector<SpecializationRequest> &requests) {
    const auto knownConstants = collectPropagatedConstants(caller);
    bool changed = false;
    auto &blocks = caller.getMutableBasicBlocks();
    for (auto &block: blocks) {
        for (size_t call = 0; call < block->instructions.size(); ++call) {
            Inst &inst = block->instructions[call];
            const auto *label = inst.op == Op::Call
                                        ? dynamic_cast<const Label *>(inst.arg1.get())
                                        : nullptr;
            auto callee = label ? callees.find(label->nameAndId) : callees.end();
            if (!label || callee == callees.end()
                || label->nameAndId.find("__const") != std::string::npos
                || instructionCost(*callee->second) > 180
                || isRecursive(*callee->second)) {
                continue;
            }
            const auto pushes = argumentPushes(block->instructions, call);
            if (pushes.size() != callee->second->getParams().size()) {
                continue;
            }

            bool hasConstant = false;
            std::vector<std::optional<int>> arguments(pushes.size());
            for (size_t index = 0; index < pushes.size(); ++index) {
                if (!callee->second->getParams()[index].dims.empty()
                    || block->instructions[pushes[index]].op != Op::PushParam) {
                    continue;
                }
                arguments[index] = constantValue(
                        block->instructions[pushes[index]].arg1.get(), knownConstants);
                hasConstant = hasConstant || arguments[index].has_value();
            }
            if (!hasConstant) {
                continue;
            }

            const std::string specializedName = specializationName(
                    label->nameAndId, arguments);
            inst.arg1 = std::make_unique<Label>(specializedName, true);
            changed = true;
            if (existing.insert(specializedName).second) {
                requests.push_back(
                        {callee->second, specializedName, std::move(arguments)});
            }
        }
    }
    return changed;
}

} // namespace

bool specializeConstantArguments(Module &module) {
    std::unordered_map<std::string, const Function *> functions;
    std::unordered_set<std::string> existing;
    for (const auto &function: module.getFunctions()) {
        functions.emplace(function->getName(), function.get());
        existing.insert(function->getName());
    }
    std::vector<SpecializationRequest> requests;
    bool changed = specializeInCaller(
            module.getMutableMainFunction(), functions, existing, requests);
    for (auto &function: module.getMutableFunctions()) {
        changed = specializeInCaller(
                          *function, functions, existing, requests)
                  || changed;
    }
    for (auto &request: requests) {
        module.addFunction(cloneSpecializedFunction(
                *request.source, request.name, request.arguments));
    }
    return changed;
}

} // namespace IR
