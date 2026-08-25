#include "middle/Optimize.h"

#include "middle/Analysis.h"

#include <algorithm>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

constexpr size_t InlineInstructionBudget = 64;
constexpr size_t CallerGrowthBudget = 256;

int nextTempId(const Function &function) {
    int next = 0;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                if (temp && temp->id >= next) {
                    next = temp->id + 1;
                }
            }
        }
    }
    return next;
}

size_t instructionCost(const Function &function) {
    size_t cost = 0;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            cost += !inst.isScopeMarker();
        }
    }
    return cost;
}

bool isInlineCandidate(const Function &function) {
    if (function.getBasicBlocks().empty()
        || function.getParams().size() > 4
        || instructionCost(function) > InlineInstructionBudget
        || std::any_of(function.getParams().begin(), function.getParams().end(), [](const ParamInfo &param) {
               return !param.dims.empty();
           })) {
        return false;
    }
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op == Op::Call || inst.op == Op::PushAddressParam) {
                return false;
            }
        }
    }
    return true;
}

struct CallSite {
    size_t block{};
    size_t frameStart{};
    size_t call{};
    size_t frameEnd{};
    size_t resultMove{};
    size_t after{};
    std::vector<size_t> pushes;
    Temp result{0, Type::Void};
    bool hasResult{false};
};

std::optional<CallSite> findCallSite(Function &caller,
                                     const Function &callee) {
    const auto &blocks = caller.getBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        const auto &instructions = blocks[block]->instructions;
        for (size_t call = 0; call < instructions.size(); ++call) {
            const auto *label = dynamic_cast<const Label *>(instructions[call].arg1.get());
            if (instructions[call].op != Op::Call || !label
                || label->nameAndId != callee.getName()) {
                continue;
            }

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
            if (frameStart == call || call + 1 >= instructions.size()
                || instructions[call + 1].op != Op::OutStack) {
                continue;
            }

            std::vector<size_t> pushes;
            nesting = 0;
            bool unsupported = false;
            for (size_t index = frameStart + 1; index < call; ++index) {
                if (instructions[index].op == Op::InStack) {
                    ++nesting;
                } else if (instructions[index].op == Op::OutStack) {
                    if (nesting == 0) {
                        unsupported = true;
                        break;
                    }
                    --nesting;
                } else if (nesting == 0 && instructions[index].op == Op::PushParam) {
                    pushes.push_back(index);
                } else if (nesting == 0 && instructions[index].op == Op::PushAddressParam) {
                    unsupported = true;
                    break;
                }
            }
            if (unsupported || nesting != 0 || pushes.size() != callee.getParams().size()) {
                continue;
            }

            CallSite site;
            site.block = block;
            site.frameStart = frameStart;
            site.call = call;
            site.frameEnd = call + 1;
            site.resultMove = instructions.size();
            site.after = call + 2;
            site.pushes = std::move(pushes);
            if (callee.getReturnType() != Type::Void) {
                if (site.after >= instructions.size()
                    || instructions[site.after].op != Op::NewMove) {
                    continue;
                }
                const auto *physical = dynamic_cast<const Temp *>(instructions[site.after].arg1.get());
                const auto *result = dynamic_cast<const Temp *>(instructions[site.after].res.get());
                if (!physical || physical->id != -2 || !result || result->id < 0) {
                    continue;
                }
                site.resultMove = site.after;
                site.result = *result;
                site.hasResult = true;
                ++site.after;
            }
            return site;
        }
    }
    return std::nullopt;
}

struct CloneContext {
    int nextTemp{};
    std::string suffix;
    std::unordered_map<int, Temp> temps;
    std::unordered_map<Var, Var> variables;
    std::unordered_map<std::string, std::string> labels;
};

std::unique_ptr<Element> cloneElement(const Element *element, CloneContext &context) {
    if (!element) {
        return nullptr;
    }
    if (const auto *temp = dynamic_cast<const Temp *>(element)) {
        if (temp->id < 0) {
            return temp->clone();
        }
        auto mapped = context.temps.find(temp->id);
        if (mapped == context.temps.end()) {
            mapped = context.temps.emplace(
                    temp->id, Temp(context.nextTemp++, temp->type)).first;
        }
        return mapped->second.clone();
    }
    if (const auto *var = dynamic_cast<const Var *>(element)) {
        if (var->depth == 0) {
            return var->clone();
        }
        auto mapped = context.variables.find(*var);
        if (mapped == context.variables.end()) {
            mapped = context.variables.emplace(
                    *var,
                    Var(var->name + context.suffix,
                        var->depth,
                        var->cons,
                        var->dims,
                        var->type,
                        var->storesAddress)).first;
        }
        return mapped->second.clone();
    }
    if (const auto *label = dynamic_cast<const Label *>(element)) {
        auto mapped = context.labels.find(label->nameAndId);
        return mapped == context.labels.end()
                       ? label->clone()
                       : std::make_unique<Label>(mapped->second, true);
    }
    return element->clone();
}

Inst cloneInstruction(const Inst &inst, CloneContext &context) {
    Inst clone(inst.op,
               cloneElement(inst.res.get(), context),
               cloneElement(inst.arg1.get(), context),
               cloneElement(inst.arg2.get(), context));
    for (const auto &incoming: inst.phiIncoming) {
        auto predecessor = context.labels.find(incoming.predecessor);
        clone.addPhiIncoming(
                predecessor == context.labels.end() ? incoming.predecessor : predecessor->second,
                cloneElement(incoming.value.get(), context));
    }
    return clone;
}

bool inlineAt(Function &caller,
              const Function &callee,
              const CallSite &site,
              size_t inlineId) {
    auto &callerBlocks = caller.getMutableBasicBlocks();
    auto &callInstructions = callerBlocks[site.block]->instructions;
    std::vector<std::unique_ptr<Element>> arguments;
    arguments.reserve(site.pushes.size());
    for (auto push = site.pushes.rbegin(); push != site.pushes.rend(); ++push) {
        arguments.push_back(callInstructions[*push].arg1->clone());
    }

    CloneContext context;
    context.nextTemp = nextTempId(caller);
    context.suffix = ".inl" + std::to_string(inlineId);

    BasicBlocks clonedBlocks;
    clonedBlocks.reserve(callee.getBasicBlocks().size());
    for (const auto &block: callee.getBasicBlocks()) {
        auto clone = std::make_unique<BasicBlock>(
                caller.getName() + "_inline_" + callee.getName());
        context.labels.emplace(block->label.nameAndId, clone->label.nameAndId);
        clonedBlocks.push_back(std::move(clone));
    }
    for (size_t block = 0; block < callee.getBasicBlocks().size(); ++block) {
        for (const auto &inst: callee.getBasicBlocks()[block]->instructions) {
            clonedBlocks[block]->instructions.push_back(cloneInstruction(inst, context));
        }
    }

    auto continuation = std::make_unique<BasicBlock>(
            caller.getName() + "_inline_cont");
    const Label continuationLabel = continuation->label;
    for (size_t index = site.after; index < callInstructions.size(); ++index) {
        continuation->instructions.push_back(std::move(callInstructions[index]));
    }

    std::optional<Var> resultVariable;
    if (site.hasResult) {
        resultVariable.emplace("__inline_result_" + std::to_string(inlineId),
                               1,
                               false,
                               std::vector<int>{},
                               site.result.type,
                               false);
        continuation->instructions.insert(
                continuation->instructions.begin(),
                Inst(Op::Load,
                     site.result.clone(),
                     resultVariable->clone(),
                     nullptr));
    }

    for (auto &block: clonedBlocks) {
        auto &instructions = block->instructions;
        for (size_t index = 0; index < instructions.size(); ++index) {
            if (instructions[index].op != Op::Ret) {
                continue;
            }
            std::vector<Inst> replacement;
            if (resultVariable && instructions[index].arg1) {
                replacement.emplace_back(
                        Op::Store,
                        instructions[index].arg1->clone(),
                        resultVariable->clone(),
                        nullptr);
            }
            replacement.emplace_back(
                    Op::Br, nullptr, continuationLabel.clone(), nullptr);
            instructions.erase(instructions.begin() + static_cast<long>(index));
            instructions.insert(instructions.begin() + static_cast<long>(index),
                                std::make_move_iterator(replacement.begin()),
                                std::make_move_iterator(replacement.end()));
            index += replacement.size() - 1;
        }
    }

    auto &entry = clonedBlocks.front()->instructions;
    std::vector<Inst> parameterSetup;
    parameterSetup.reserve(callee.getParams().size() * 2);
    for (size_t index = 0; index < callee.getParams().size(); ++index) {
        const auto &param = callee.getParams()[index];
        Var formal(param.name, 1, false, param.dims, param.type, false);
        auto clonedFormal = cloneElement(&formal, context);
        parameterSetup.emplace_back(
                Op::Alloca,
                nullptr,
                clonedFormal->clone(),
                std::make_unique<ConstVal>(1, Type::Int));
        parameterSetup.emplace_back(
                Op::Store,
                std::move(arguments[index]),
                std::move(clonedFormal),
                nullptr);
    }
    entry.insert(entry.begin(),
                 std::make_move_iterator(parameterSetup.begin()),
                 std::make_move_iterator(parameterSetup.end()));

    std::unordered_set<size_t> removed(site.pushes.begin(), site.pushes.end());
    removed.insert(site.frameStart);
    removed.insert(site.call);
    removed.insert(site.frameEnd);
    if (site.hasResult) {
        removed.insert(site.resultMove);
    }
    std::vector<Inst> prefix;
    prefix.reserve(site.call + 3);
    for (size_t index = 0; index < site.after; ++index) {
        if (removed.find(index) == removed.end()) {
            prefix.push_back(std::move(callInstructions[index]));
        }
    }
    if (resultVariable) {
        prefix.emplace_back(
                Op::Alloca,
                nullptr,
                resultVariable->clone(),
                std::make_unique<ConstVal>(1, Type::Int));
    }
    prefix.emplace_back(
            Op::Br, nullptr, clonedBlocks.front()->label.clone(), nullptr);
    callInstructions = std::move(prefix);

    const size_t insertAt = site.block + 1;
    callerBlocks.insert(callerBlocks.begin() + static_cast<long>(insertAt),
                        std::make_move_iterator(clonedBlocks.begin()),
                        std::make_move_iterator(clonedBlocks.end()));
    callerBlocks.insert(callerBlocks.begin()
                                + static_cast<long>(insertAt + callee.getBasicBlocks().size()),
                        std::move(continuation));
    return true;
}

} // namespace

bool inlineFunctions(Module &module) {
    std::unordered_map<std::string, const Function *> candidates;
    for (const auto &function: module.getFunctions()) {
        if (isInlineCandidate(*function)) {
            candidates.emplace(function->getName(), function.get());
        }
    }
    if (candidates.empty()) {
        return false;
    }

    static size_t nextInlineId = 0;
    auto tryCaller = [&](Function &caller) {
        const size_t callerCost = instructionCost(caller);
        for (const auto &[name, callee]: candidates) {
            if (name == caller.getName()) {
                continue;
            }
            auto site = findCallSite(caller, *callee);
            const size_t cost = instructionCost(*callee);
            if (site && callerCost + cost <= CallerGrowthBudget) {
                inlineAt(caller, *callee, *site, nextInlineId++);
                return true;
            }
        }
        return false;
    };

    if (tryCaller(module.getMutableMainFunction())) {
        return true;
    }
    for (auto &function: module.getMutableFunctions()) {
        if (tryCaller(*function)) {
            return true;
        }
    }
    return false;
}

bool eliminateUnreachableFunctions(Module &module) {
    std::unordered_map<std::string, const Function *> functions;
    for (const auto &function: module.getFunctions()) {
        functions.emplace(function->getName(), function.get());
    }

    std::unordered_set<std::string> reachable;
    std::queue<const Function *> work;
    auto discoverCalls = [&](const Function &caller) {
        for (const auto &block: caller.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                const auto *callee = inst.op == Op::Call
                                             ? dynamic_cast<const Label *>(inst.arg1.get())
                                             : nullptr;
                auto function = callee ? functions.find(callee->nameAndId) : functions.end();
                if (function != functions.end()
                    && reachable.insert(function->first).second) {
                    work.push(function->second);
                }
            }
        }
    };

    discoverCalls(module.getMainFunction());
    while (!work.empty()) {
        const Function *function = work.front();
        work.pop();
        discoverCalls(*function);
    }

    auto &mutableFunctions = module.getMutableFunctions();
    const size_t oldSize = mutableFunctions.size();
    mutableFunctions.erase(
            std::remove_if(mutableFunctions.begin(), mutableFunctions.end(), [&](const auto &function) {
                return reachable.find(function->getName()) == reachable.end();
            }),
            mutableFunctions.end());
    return mutableFunctions.size() != oldSize;
}

} // namespace IR
