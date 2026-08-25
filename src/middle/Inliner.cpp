#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <cstdint>
#include <limits>
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
                } else if (nesting == 0
                           && (instructions[index].op == Op::PushParam
                               || instructions[index].op == Op::PushAddressParam)) {
                    pushes.push_back(index);
                }
            }
            if (unsupported || nesting != 0 || pushes.size() != callee.getParams().size()) {
                continue;
            }
            bool parameterKindsMatch = true;
            for (size_t index = 0; index < pushes.size(); ++index) {
                const auto &formal = callee.getParams()[index];
                const Op pushOp = instructions[pushes[pushes.size() - index - 1]].op;
                if ((formal.dims.empty() && pushOp != Op::PushParam)
                    || (!formal.dims.empty() && pushOp != Op::PushAddressParam)) {
                    parameterKindsMatch = false;
                    break;
                }
            }
            if (!parameterKindsMatch) {
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
    std::unordered_map<int, const Element *> parameterValues;
    std::unordered_map<Var, Var> variables;
    std::unordered_map<std::string, std::string> labels;
};

std::unique_ptr<Element> cloneElement(const Element *element, CloneContext &context) {
    if (!element) {
        return nullptr;
    }
    if (const auto *temp = dynamic_cast<const Temp *>(element)) {
        auto parameter = context.parameterValues.find(temp->id);
        if (parameter != context.parameterValues.end()) {
            return parameter->second->clone();
        }
        if (temp->id < 0) {
            return temp->clone();
        }
        auto mapped = context.temps.find(temp->id);
        if (mapped == context.temps.end()) {
            mapped = context.temps.emplace(
                                          temp->id, Temp(context.nextTemp++, temp->type))
                             .first;
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
                                                  var->storesAddress))
                             .first;
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

bool inlineTrivialAt(Function &caller,
                     const Function &callee,
                     const CallSite &site,
                     size_t inlineId) {
    CloneContext context;
    context.nextTemp = nextTempId(caller);
    context.suffix = ".tinl" + std::to_string(inlineId);

    std::vector<Inst> body;
    for (const auto &inst: callee.getBasicBlocks().front()->instructions) {
        if (inst.op != Op::Ret) {
            body.push_back(cloneInstruction(inst, context));
        }
    }

    auto &instructions = caller.getMutableBasicBlocks()[site.block]->instructions;
    std::unordered_set<size_t> removed(site.pushes.begin(), site.pushes.end());
    removed.insert(site.frameStart);
    removed.insert(site.call);
    removed.insert(site.frameEnd);
    std::vector<Inst> replacement;
    replacement.reserve(instructions.size() + body.size());
    for (size_t index = 0; index < instructions.size(); ++index) {
        if (index == site.call) {
            replacement.insert(
                    replacement.end(),
                    std::make_move_iterator(body.begin()),
                    std::make_move_iterator(body.end()));
        }
        if (removed.count(index) == 0) {
            replacement.push_back(std::move(instructions[index]));
        }
    }
    instructions = std::move(replacement);
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

bool inlineTrivialFunctions(Module &module) {
    std::unordered_map<std::string, const Function *> candidates;
    for (const auto &function: module.getFunctions()) {
        if (function->getReturnType() == Type::Void
            && function->getParams().empty()
            && function->getBasicBlocks().size() == 1
            && instructionCost(*function) <= 16
            && std::none_of(
                    function->getBasicBlocks().front()->instructions.begin(),
                    function->getBasicBlocks().front()->instructions.end(),
                    [](const Inst &inst) {
                        return inst.op == Op::Call || inst.op == Op::Phi
                               || inst.op == Op::Parameter;
                    })) {
            candidates.emplace(function->getName(), function.get());
        }
    }
    if (candidates.empty()) {
        return false;
    }

    static size_t nextTrivialInlineId = 1000000;
    auto tryCaller = [&](Function &caller) {
        for (const auto &[name, callee]: candidates) {
            if (name == caller.getName()) {
                continue;
            }
            auto site = findCallSite(caller, *callee);
            if (site) {
                inlineTrivialAt(caller, *callee, *site, nextTrivialInlineId++);
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

bool isLinearInlineCandidate(const Function &function) {
    if (function.getBasicBlocks().empty()
        || function.getParams().size() > 4
        || instructionCost(function) > 40
        || std::any_of(
                function.getParams().begin(), function.getParams().end(),
                [](const ParamInfo &param) { return !param.dims.empty(); })) {
        return false;
    }
    const auto cfg = buildControlFlowGraph(function);
    int returns = 0;
    for (size_t block = 0; block < function.getBasicBlocks().size(); ++block) {
        if (!cfg.reachable[block]) {
            return false;
        }
        if (block + 1 < function.getBasicBlocks().size()) {
            if (cfg.successors[block].size() != 1
                || cfg.successors[block].front() != block + 1) {
                return false;
            }
        } else if (!cfg.successors[block].empty()) {
            return false;
        }
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            if (inst.op == Op::Ret) {
                ++returns;
            } else if (inst.op == Op::Bif0 || inst.op == Op::Bif1
                       || inst.op == Op::Phi || inst.op == Op::Call
                       || inst.op == Op::PushParam
                       || inst.op == Op::PushAddressParam) {
                return false;
            }
        }
    }
    return returns == 1;
}

bool inlineLinearAt(Function &caller,
                    const Function &callee,
                    const CallSite &site,
                    size_t inlineId) {
    auto &instructions = caller.getMutableBasicBlocks()[site.block]->instructions;
    std::vector<std::unique_ptr<Element>> arguments;
    arguments.reserve(site.pushes.size());
    for (auto push = site.pushes.rbegin(); push != site.pushes.rend(); ++push) {
        arguments.push_back(instructions[*push].arg1->clone());
    }

    CloneContext context;
    context.nextTemp = nextTempId(caller);
    context.suffix = ".linl" + std::to_string(inlineId);
    for (const auto &block: callee.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != Op::Parameter) {
                continue;
            }
            const auto *result = dynamic_cast<const Temp *>(inst.res.get());
            const auto *formal = dynamic_cast<const Var *>(inst.arg1.get());
            if (!result || !formal) {
                return false;
            }
            auto parameter = std::find_if(
                    callee.getParams().begin(), callee.getParams().end(),
                    [&](const ParamInfo &candidate) {
                        return candidate.name == formal->name;
                    });
            if (parameter == callee.getParams().end()) {
                return false;
            }
            const size_t index = static_cast<size_t>(
                    std::distance(callee.getParams().begin(), parameter));
            if (index >= arguments.size()) {
                return false;
            }
            context.parameterValues[result->id] = arguments[index].get();
        }
    }

    std::vector<Inst> body;
    std::unique_ptr<Element> returnValue;
    for (const auto &block: callee.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op == Op::Ret) {
                returnValue = cloneElement(inst.arg1.get(), context);
            } else if (inst.op != Op::Parameter && inst.op != Op::Br
                       && !inst.isScopeMarker()) {
                body.push_back(cloneInstruction(inst, context));
            }
        }
    }
    if (callee.getReturnType() != Type::Void && (!site.hasResult || !returnValue)) {
        return false;
    }
    if (site.hasResult) {
        body.emplace_back(
                Op::NewMove, site.result.clone(), std::move(returnValue), nullptr);
    }

    std::unordered_set<size_t> removed(site.pushes.begin(), site.pushes.end());
    removed.insert(site.frameStart);
    removed.insert(site.call);
    removed.insert(site.frameEnd);
    if (site.hasResult) {
        removed.insert(site.resultMove);
    }
    std::vector<Inst> replacement;
    replacement.reserve(instructions.size() + body.size());
    for (size_t index = 0; index < instructions.size(); ++index) {
        if (index == site.call) {
            replacement.insert(
                    replacement.end(),
                    std::make_move_iterator(body.begin()),
                    std::make_move_iterator(body.end()));
        }
        if (removed.count(index) == 0) {
            replacement.push_back(std::move(instructions[index]));
        }
    }
    instructions = std::move(replacement);
    return true;
}

bool inlineLinearFunctions(Module &module) {
    std::unordered_map<std::string, const Function *> candidates;
    for (const auto &function: module.getFunctions()) {
        if (isLinearInlineCandidate(*function)) {
            candidates.emplace(function->getName(), function.get());
        }
    }
    if (candidates.empty()) {
        return false;
    }

    static size_t nextLinearInlineId = 2000000;
    auto tryCaller = [&](Function &caller) {
        for (const auto &[name, callee]: candidates) {
            if (name == caller.getName()) {
                continue;
            }
            auto site = findCallSite(caller, *callee);
            if (site && inlineLinearAt(caller, *callee, *site, nextLinearInlineId++)) {
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

namespace {

struct ArrayBinding {
    Var base;
    std::optional<Temp> dynamicByteOffset;
    int byteOffset{};
};

std::optional<int> inlineConstant(
        const Element *element,
        const std::unordered_map<int, int> &constants) {
    if (!element) {
        return 0;
    }
    if (const auto *value = dynamic_cast<const ConstVal *>(element)) {
        return value->value;
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    auto value = temp ? constants.find(temp->id) : constants.end();
    return value == constants.end() ? std::nullopt
                                    : std::optional<int>(value->second);
}

std::unordered_map<int, int> inlineConstants(
        const Function &function,
        std::unordered_map<int, int> result = {}) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
                if (!destination || result.count(destination->id) != 0) {
                    continue;
                }
                if (inst.op == Op::LoadImd) {
                    const auto *value = dynamic_cast<const ConstVal *>(inst.arg1.get());
                    if (value) {
                        result.emplace(destination->id, value->value);
                        changed = true;
                    }
                    continue;
                }

                const auto lhs = inlineConstant(inst.arg1.get(), result);
                const auto rhs = inlineConstant(inst.arg2.get(), result);
                std::optional<int> value;
                if (inst.op == Op::NewMove && lhs) {
                    value = *lhs;
                } else if (inst.op == Op::Neg && lhs) {
                    value = static_cast<int>(0U - static_cast<std::uint32_t>(*lhs));
                } else if (inst.op == Op::Mult4 && lhs) {
                    value = static_cast<int>(static_cast<std::uint32_t>(*lhs) * 4U);
                } else if ((inst.op == Op::Add || inst.op == Op::Sub
                            || inst.op == Op::Mul || inst.op == Op::MulImd)
                           && lhs && rhs) {
                    const std::uint32_t left = static_cast<std::uint32_t>(*lhs);
                    const std::uint32_t right = static_cast<std::uint32_t>(*rhs);
                    if (inst.op == Op::Add) {
                        value = static_cast<int>(left + right);
                    } else if (inst.op == Op::Sub) {
                        value = static_cast<int>(left - right);
                    } else {
                        value = static_cast<int>(left * right);
                    }
                }
                if (value) {
                    if (destination->type == Type::Char) {
                        *value &= 0xFF;
                    }
                    if (result.emplace(destination->id, *value).second) {
                        changed = true;
                    }
                }
            }
        }
    }
    return result;
}

bool isLinearArrayInlineCandidate(const Function &function) {
    if (function.getBasicBlocks().empty()
        || function.getParams().size() > 8
        || instructionCost(function) > 96
        || std::none_of(
                function.getParams().begin(), function.getParams().end(),
                [](const ParamInfo &param) { return !param.dims.empty(); })) {
        return false;
    }
    int returns = 0;
    const auto cfg = buildControlFlowGraph(function);
    for (size_t block = 0; block < function.getBasicBlocks().size(); ++block) {
        if (!cfg.reachable[block]) {
            return false;
        }
        if (block + 1 < function.getBasicBlocks().size()) {
            if (cfg.successors[block].size() != 1
                || cfg.successors[block].front() != block + 1) {
                return false;
            }
        } else if (!cfg.successors[block].empty()) {
            return false;
        }
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            if (inst.op == Op::Ret) {
                ++returns;
            } else if (inst.op == Op::Bif0 || inst.op == Op::Bif1
                       || inst.op == Op::Phi || inst.op == Op::Call
                       || inst.op == Op::PushParam
                       || inst.op == Op::PushAddressParam) {
                return false;
            }
        }
    }
    return returns == 1;
}

bool addByteOffset(ArrayBinding &binding, std::int64_t delta) {
    const std::int64_t result = static_cast<std::int64_t>(binding.byteOffset) + delta;
    if (result < std::numeric_limits<int>::min()
        || result > std::numeric_limits<int>::max()) {
        return false;
    }
    binding.byteOffset = static_cast<int>(result);
    return true;
}

std::optional<int> arrayElementOffset(const ArrayBinding &binding) {
    if (binding.dynamicByteOffset) {
        return std::nullopt;
    }
    const int elementSize = sizeOfType(ptrToValue(binding.base.type));
    if (elementSize <= 0 || binding.byteOffset % elementSize != 0) {
        return std::nullopt;
    }
    return binding.byteOffset / elementSize;
}

bool addDynamicByteOffset(
        ArrayBinding &binding,
        const Element *offset,
        bool subtract,
        CloneContext &context,
        std::vector<Inst> &body) {
    auto cloned = cloneElement(offset, context);
    const auto *temp = dynamic_cast<const Temp *>(cloned.get());
    if (!temp || temp->type != Type::Int) {
        return false;
    }
    if (!binding.dynamicByteOffset && !subtract) {
        binding.dynamicByteOffset = *temp;
        return true;
    }

    const Temp combined(context.nextTemp++, Type::Int);
    if (binding.dynamicByteOffset) {
        body.emplace_back(
                subtract ? Op::Sub : Op::Add,
                combined.clone(),
                binding.dynamicByteOffset->clone(),
                temp->clone());
    } else {
        body.emplace_back(
                Op::Neg, combined.clone(), temp->clone(), nullptr);
    }
    binding.dynamicByteOffset = combined;
    return true;
}

bool extendArrayBinding(
        ArrayBinding &binding,
        const Element *offset,
        bool byteOffset,
        bool subtract,
        int elementSize,
        const std::unordered_map<int, int> &constants,
        CloneContext &context,
        std::vector<Inst> &body) {
    const auto constant = inlineConstant(offset, constants);
    if (constant) {
        const std::int64_t scale = byteOffset ? 1 : elementSize;
        const std::int64_t delta = static_cast<std::int64_t>(*constant) * scale;
        return addByteOffset(binding, subtract ? -delta : delta);
    }
    return byteOffset
           && addDynamicByteOffset(binding, offset, subtract, context, body);
}

bool emitBoundMemoryOperation(
        const Inst &inst,
        ArrayBinding binding,
        CloneContext &context,
        std::vector<Inst> &body) {
    if (!binding.dynamicByteOffset) {
        const auto elementOffset = arrayElementOffset(binding);
        if (!elementOffset) {
            return false;
        }
        body.emplace_back(
                inst.op == Op::Store || inst.op == Op::StoreDynamic
                        ? Op::Store
                        : Op::Load,
                cloneElement(inst.res.get(), context),
                binding.base.clone(),
                std::make_unique<ConstVal>(*elementOffset, Type::Int));
        return true;
    }

    Temp offset = *binding.dynamicByteOffset;
    if (binding.byteOffset != 0) {
        const Temp adjusted(context.nextTemp++, Type::Int);
        body.emplace_back(
                Op::Add,
                adjusted.clone(),
                offset.clone(),
                std::make_unique<ConstVal>(binding.byteOffset, Type::Int));
        offset = adjusted;
    }
    body.emplace_back(
            inst.op == Op::Store || inst.op == Op::StoreDynamic
                    ? Op::StoreDynamic
                    : Op::LoadDynamic,
            cloneElement(inst.res.get(), context),
            binding.base.clone(),
            offset.clone());
    return true;
}

bool inlineLinearArrayAt(Function &caller,
                         const Function &callee,
                         const CallSite &site,
                         size_t inlineId) {
    auto &instructions = caller.getMutableBasicBlocks()[site.block]->instructions;
    const auto callerConstants = inlineConstants(caller);
    std::vector<std::unique_ptr<Element>> scalarArguments(callee.getParams().size());
    std::unordered_map<std::string, ArrayBinding> arrayArguments;
    for (size_t index = 0; index < callee.getParams().size(); ++index) {
        const auto &push = instructions[site.pushes[site.pushes.size() - index - 1]];
        const auto &formal = callee.getParams()[index];
        if (formal.dims.empty()) {
            scalarArguments[index] = push.arg1->clone();
            continue;
        }
        const auto *base = dynamic_cast<const Var *>(push.arg1.get());
        if (!base) {
            return false;
        }
        ArrayBinding binding{*base, std::nullopt, 0};
        const int elementSize = sizeOfType(ptrToValue(base->type));
        const bool byteOffset = dynamic_cast<const Temp *>(push.arg2.get()) != nullptr;
        const auto offset = inlineConstant(push.arg2.get(), callerConstants);
        if (elementSize <= 0) {
            return false;
        }
        if (offset) {
            if (!addByteOffset(
                        binding,
                        byteOffset
                                ? static_cast<std::int64_t>(*offset)
                                : static_cast<std::int64_t>(*offset) * elementSize)) {
                return false;
            }
        } else {
            const auto *dynamic = dynamic_cast<const Temp *>(push.arg2.get());
            if (!byteOffset || !dynamic || dynamic->type != Type::Int) {
                return false;
            }
            binding.dynamicByteOffset = *dynamic;
        }
        arrayArguments.emplace(formal.name, std::move(binding));
    }

    CloneContext context;
    context.nextTemp = nextTempId(caller);
    context.suffix = ".ainl" + std::to_string(inlineId);
    std::unordered_map<int, int> calleeConstants;
    for (const auto &block: callee.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != Op::Parameter) {
                continue;
            }
            const auto *result = dynamic_cast<const Temp *>(inst.res.get());
            const auto *formal = dynamic_cast<const Var *>(inst.arg1.get());
            auto parameter = formal
                                     ? std::find_if(
                                               callee.getParams().begin(),
                                               callee.getParams().end(),
                                               [&](const ParamInfo &candidate) {
                                                   return candidate.name == formal->name;
                                               })
                                     : callee.getParams().end();
            if (!result || parameter == callee.getParams().end()) {
                return false;
            }
            const size_t index = static_cast<size_t>(
                    std::distance(callee.getParams().begin(), parameter));
            if (!scalarArguments[index]) {
                return false;
            }
            context.parameterValues[result->id] = scalarArguments[index].get();
            if (const auto value = inlineConstant(
                        scalarArguments[index].get(), callerConstants)) {
                calleeConstants[result->id] = *value;
            }
        }
    }

    const auto constants = inlineConstants(callee, std::move(calleeConstants));
    std::unordered_map<int, ArrayBinding> pointerValues;
    std::vector<Inst> body;
    std::unique_ptr<Element> returnValue;
    for (const auto &block: callee.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op == Op::Parameter || inst.isScopeMarker()) {
                continue;
            }
            if (inst.op == Op::Br) {
                continue;
            }
            if (inst.op == Op::Ret) {
                returnValue = cloneElement(inst.arg1.get(), context);
                continue;
            }

            const auto *formal = dynamic_cast<const Var *>(inst.arg1.get());
            auto array = formal ? arrayArguments.find(formal->name) : arrayArguments.end();
            if ((inst.op == Op::Load || inst.op == Op::LoadDynamic)
                && array != arrayArguments.end()) {
                const auto *result = dynamic_cast<const Temp *>(inst.res.get());
                if (!result) {
                    return false;
                }
                if (!inst.arg2) {
                    pointerValues.insert_or_assign(result->id, array->second);
                    continue;
                }
                ArrayBinding binding = array->second;
                const int elementSize = sizeOfType(ptrToValue(formal->type));
                const bool byteOffset = inst.op == Op::LoadDynamic;
                if (elementSize <= 0
                    || !extendArrayBinding(
                            binding, inst.arg2.get(), byteOffset, false,
                            elementSize, constants, context, body)) {
                    return false;
                }
                if (!emitBoundMemoryOperation(inst, std::move(binding), context, body)) {
                    return false;
                }
                continue;
            }
            if ((inst.op == Op::Store || inst.op == Op::StoreDynamic)
                && array != arrayArguments.end()) {
                ArrayBinding binding = array->second;
                const int elementSize = sizeOfType(ptrToValue(formal->type));
                const bool byteOffset = inst.op == Op::StoreDynamic;
                if (elementSize <= 0
                    || !extendArrayBinding(
                            binding, inst.arg2.get(), byteOffset, false,
                            elementSize, constants, context, body)) {
                    return false;
                }
                if (!emitBoundMemoryOperation(inst, std::move(binding), context, body)) {
                    return false;
                }
                continue;
            }

            const auto *lhs = dynamic_cast<const Temp *>(inst.arg1.get());
            const auto *rhs = dynamic_cast<const Temp *>(inst.arg2.get());
            auto lhsPointer = lhs ? pointerValues.find(lhs->id) : pointerValues.end();
            auto rhsPointer = rhs ? pointerValues.find(rhs->id) : pointerValues.end();
            const auto *result = dynamic_cast<const Temp *>(inst.res.get());
            if (result && (inst.op == Op::Add || inst.op == Op::Sub)
                && (lhsPointer != pointerValues.end()
                    || (inst.op == Op::Add && rhsPointer != pointerValues.end()))) {
                const bool pointerOnLeft = lhsPointer != pointerValues.end();
                ArrayBinding binding = pointerOnLeft ? lhsPointer->second : rhsPointer->second;
                const Element *offset = pointerOnLeft ? inst.arg2.get() : inst.arg1.get();
                if (!extendArrayBinding(
                            binding, offset, true, inst.op == Op::Sub,
                            1, constants, context, body)) {
                    return false;
                }
                pointerValues.insert_or_assign(result->id, std::move(binding));
                continue;
            }
            if (inst.op == Op::NewMove && result && lhsPointer != pointerValues.end()) {
                pointerValues.insert_or_assign(result->id, lhsPointer->second);
                continue;
            }
            if (inst.op == Op::LoadPtr && lhsPointer != pointerValues.end()) {
                const auto *loaded = dynamic_cast<const Temp *>(inst.res.get());
                if (!loaded) {
                    return false;
                }
                ArrayBinding binding = lhsPointer->second;
                const int elementSize = sizeOfType(loaded->type);
                if (elementSize <= 0
                    || !extendArrayBinding(
                            binding, inst.arg2.get(), false, false,
                            elementSize, constants, context, body)) {
                    return false;
                }
                if (!emitBoundMemoryOperation(inst, std::move(binding), context, body)) {
                    return false;
                }
                continue;
            }

            bool usesPointerValue = false;
            bool usesArrayFormal = false;
            for (const auto &operand: inst.operands()) {
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                const auto *var = dynamic_cast<const Var *>(operand.value);
                usesPointerValue = usesPointerValue
                                   || (temp && pointerValues.count(temp->id) != 0);
                usesArrayFormal = usesArrayFormal
                                  || (var && arrayArguments.count(var->name) != 0);
            }
            if (usesPointerValue || usesArrayFormal) {
                return false;
            }
            body.push_back(cloneInstruction(inst, context));
        }
    }

    if (callee.getReturnType() != Type::Void && (!site.hasResult || !returnValue)) {
        return false;
    }
    if (site.hasResult) {
        body.emplace_back(
                Op::NewMove, site.result.clone(), std::move(returnValue), nullptr);
    }

    std::unordered_set<size_t> removed(site.pushes.begin(), site.pushes.end());
    removed.insert(site.frameStart);
    removed.insert(site.call);
    removed.insert(site.frameEnd);
    if (site.hasResult) {
        removed.insert(site.resultMove);
    }
    std::vector<Inst> replacement;
    replacement.reserve(instructions.size() + body.size());
    for (size_t index = 0; index < instructions.size(); ++index) {
        if (index == site.call) {
            replacement.insert(
                    replacement.end(),
                    std::make_move_iterator(body.begin()),
                    std::make_move_iterator(body.end()));
        }
        if (removed.count(index) == 0) {
            replacement.push_back(std::move(instructions[index]));
        }
    }
    instructions = std::move(replacement);
    return true;
}

} // namespace

bool inlineLinearArrayFunctions(Module &module) {
    std::unordered_map<std::string, const Function *> candidates;
    for (const auto &function: module.getFunctions()) {
        if (isLinearArrayInlineCandidate(*function)) {
            candidates.emplace(function->getName(), function.get());
        }
    }
    if (candidates.empty()) {
        return false;
    }

    static size_t nextArrayInlineId = 3000000;
    bool changed = false;
    for (int iteration = 0; iteration < 128; ++iteration) {
        bool inlined = false;
        auto tryCaller = [&](Function &caller) {
            for (const auto &[name, callee]: candidates) {
                if (name == caller.getName()) {
                    continue;
                }
                auto site = findCallSite(caller, *callee);
                if (site) {
                    const bool success = inlineLinearArrayAt(
                            caller, *callee, *site, nextArrayInlineId++);
                    if (success) {
                        return true;
                    }
                }
            }
            return false;
        };
        inlined = tryCaller(module.getMutableMainFunction());
        if (!inlined) {
            for (auto &function: module.getMutableFunctions()) {
                if (tryCaller(*function)) {
                    inlined = true;
                    break;
                }
            }
        }
        if (!inlined) {
            break;
        }
        changed = true;
    }
    return changed;
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
