#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

struct FunctionSummary {
    const Function *function{};
    bool directSideEffects{};
    bool sideEffectFree{};
    bool readsExternalMemory{};
    bool terminating{};
    std::optional<int> constantReturn;
    std::vector<std::string> callees;
};

struct CallFrame {
    size_t block{};
    size_t frameStart{};
    size_t call{};
    size_t frameEnd{};
    size_t resultMove{};
    std::vector<size_t> pushes;
    std::vector<const Element *> arguments;
    const Temp *result{};
};

bool accessesExternalMemory(const Inst &inst) {
    if (inst.op != Op::Load && inst.op != Op::LoadDynamic
        && inst.op != Op::LoadPtr && inst.op != Op::Store
        && inst.op != Op::StoreDynamic && inst.op != Op::MemZero) {
        return false;
    }
    const auto *var = dynamic_cast<const Var *>(inst.arg1.get());
    return !var || var->depth == 0 || var->storesAddress;
}

bool directlyObservable(const Inst &inst) {
    switch (inst.op) {
        case Op::GetInt:
        case Op::GetChar:
        case Op::GetString:
        case Op::PrintInt:
        case Op::PrintChar:
        case Op::PrintStr:
            return true;
        case Op::Store:
        case Op::StoreDynamic:
        case Op::MemZero:
            return accessesExternalMemory(inst);
        default:
            return false;
    }
}

std::optional<int> commonReturnConstant(const Function &function) {
    const auto constants = collectPropagatedConstants(function);
    std::optional<int> result;
    bool sawReturn = false;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != Op::Ret) {
                continue;
            }
            sawReturn = true;
            const auto value = constantValue(inst.arg1.get(), constants);
            if (!value || (result && *result != *value)) {
                return std::nullopt;
            }
            result = value;
        }
    }
    return sawReturn ? result : std::nullopt;
}

bool hasAcyclicControlFlow(const Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    for (size_t block = 0; block < cfg.successors.size(); ++block) {
        for (size_t successor: cfg.successors[block]) {
            if (cfg.dominates(successor, block)) {
                return false;
            }
        }
    }
    return true;
}

bool proveMonotoneLoopTermination(const Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto defs = collectTempDefinitions(function);
    const auto constants = collectPropagatedConstants(function);
    const auto &blocks = function.getBasicBlocks();

    size_t header = ControlFlowGraph::NoBlock;
    for (size_t source = 0; source < cfg.successors.size(); ++source) {
        for (size_t successor: cfg.successors[source]) {
            if (!cfg.dominates(successor, source)) {
                continue;
            }
            if (header != ControlFlowGraph::NoBlock && header != successor) {
                return false;
            }
            header = successor;
        }
    }
    if (header == ControlFlowGraph::NoBlock) {
        return false;
    }

    const Inst *inductionPhi = nullptr;
    const Temp *induction = nullptr;
    int maximumStep = 0;
    for (const auto &phi: blocks[header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *candidate = dynamic_cast<const Temp *>(phi.res.get());
        if (!candidate || candidate->type != Type::Int) {
            continue;
        }
        bool hasEntry = false;
        bool validUpdates = true;
        int candidateMaximumStep = 0;
        for (const auto &incoming: phi.phiIncoming) {
            auto predecessor = std::find_if(
                    blocks.begin(), blocks.end(), [&](const auto &block) {
                        return block->label.nameAndId == incoming.predecessor;
                    });
            if (predecessor == blocks.end()) {
                validUpdates = false;
                break;
            }
            const size_t predecessorIndex = static_cast<size_t>(
                    std::distance(blocks.begin(), predecessor));
            if (!cfg.dominates(header, predecessorIndex)) {
                const auto *initial = dynamic_cast<const Temp *>(incoming.value.get());
                auto initialDefinition = initial ? defs.find(initial->id) : defs.end();
                hasEntry = initialDefinition != defs.end()
                           && blocks[initialDefinition->second.block]
                                              ->instructions[initialDefinition->second.instruction]
                                              .op
                                      == Op::Parameter;
                continue;
            }
            const auto *updated = dynamic_cast<const Temp *>(incoming.value.get());
            auto updateDefinition = updated ? defs.find(updated->id) : defs.end();
            if (updateDefinition == defs.end()) {
                validUpdates = false;
                break;
            }
            const Inst &update = blocks[updateDefinition->second.block]
                                         ->instructions[updateDefinition->second.instruction];
            const auto step = update.op == Op::Sub
                                              && sameTemp(update.arg1.get(), candidate->id)
                                      ? constantValue(update.arg2.get(), constants)
                                      : std::nullopt;
            if (!step || *step <= 0) {
                validUpdates = false;
                break;
            }
            candidateMaximumStep = std::max(candidateMaximumStep, *step);
        }
        if (hasEntry && validUpdates && candidateMaximumStep > 0) {
            inductionPhi = &phi;
            induction = candidate;
            maximumStep = candidateMaximumStep;
            break;
        }
    }
    if (!inductionPhi || !induction) {
        return false;
    }

    for (const auto &inst: blocks[header]->instructions) {
        if (inst.op != Op::Leq || !sameTemp(inst.arg1.get(), induction->id)) {
            continue;
        }
        const auto bound = constantValue(inst.arg2.get(), constants);
        const auto *condition = dynamic_cast<const Temp *>(inst.res.get());
        if (!bound || !condition
            || static_cast<long long>(*bound) + 1 - maximumStep
                       < std::numeric_limits<int>::min()) {
            continue;
        }

        const Inst *conditional = nullptr;
        const Inst *fallthrough = nullptr;
        for (const auto &branch: blocks[header]->instructions) {
            if ((branch.op == Op::Bif0 || branch.op == Op::Bif1)
                && sameTemp(branch.arg1.get(), condition->id)) {
                conditional = &branch;
            } else if (branch.op == Op::Br) {
                fallthrough = &branch;
            }
        }
        const auto *taken = conditional ? asLabel(conditional->arg2) : nullptr;
        const auto *other = fallthrough ? asLabel(fallthrough->arg1) : nullptr;
        if (!taken || !other) {
            continue;
        }
        const std::string &trueTarget = conditional->op == Op::Bif1
                                                ? taken->nameAndId
                                                : other->nameAndId;
        auto trueBlock = std::find_if(
                blocks.begin(), blocks.end(), [&](const auto &block) {
                    return block->label.nameAndId == trueTarget;
                });
        if (trueBlock == blocks.end()) {
            continue;
        }
        const size_t trueIndex = static_cast<size_t>(
                std::distance(blocks.begin(), trueBlock));
        bool reachesReturn = false;
        for (size_t block = 0; block < blocks.size(); ++block) {
            if (!cfg.dominates(trueIndex, block)) {
                continue;
            }
            reachesReturn = reachesReturn
                            || std::any_of(
                                    blocks[block]->instructions.begin(),
                                    blocks[block]->instructions.end(),
                                    [](const Inst &candidate) {
                                        return candidate.op == Op::Ret;
                                    });
        }
        if (reachesReturn) {
            return true;
        }
    }
    return false;
}

std::unordered_map<std::string, FunctionSummary> summarize(const Module &module) {
    std::unordered_map<std::string, FunctionSummary> summaries;
    for (const auto &function: module.getFunctions()) {
        FunctionSummary summary;
        summary.function = function.get();
        summary.sideEffectFree = true;
        summary.constantReturn = commonReturnConstant(*function);
        summary.terminating = hasAcyclicControlFlow(*function)
                              || proveMonotoneLoopTermination(*function);
        for (const auto &block: function->getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                summary.directSideEffects = summary.directSideEffects
                                            || directlyObservable(inst);
                if ((inst.op == Op::Load || inst.op == Op::LoadDynamic
                     || inst.op == Op::LoadPtr)
                    && accessesExternalMemory(inst)) {
                    summary.readsExternalMemory = true;
                }
                if (inst.op == Op::Call) {
                    const auto *callee = asLabel(inst.arg1);
                    if (callee) {
                        summary.callees.push_back(callee->nameAndId);
                    } else {
                        summary.directSideEffects = true;
                    }
                }
            }
        }
        summary.sideEffectFree = !summary.directSideEffects;
        summaries.emplace(function->getName(), std::move(summary));
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &[name, summary]: summaries) {
            bool sideEffectFree = !summary.directSideEffects;
            bool readsExternal = summary.readsExternalMemory;
            bool terminating = summary.terminating;
            for (const std::string &calleeName: summary.callees) {
                auto callee = summaries.find(calleeName);
                if (callee == summaries.end()) {
                    sideEffectFree = false;
                    terminating = false;
                    continue;
                }
                sideEffectFree = sideEffectFree && callee->second.sideEffectFree;
                readsExternal = readsExternal || callee->second.readsExternalMemory;
                if (calleeName != name) {
                    terminating = terminating && callee->second.terminating;
                }
            }
            if (summary.sideEffectFree != sideEffectFree
                || summary.readsExternalMemory != readsExternal
                || summary.terminating != terminating) {
                summary.sideEffectFree = sideEffectFree;
                summary.readsExternalMemory = readsExternal;
                summary.terminating = terminating;
                changed = true;
            }
        }
    }
    return summaries;
}

std::optional<CallFrame> callFrame(
        Function &caller,
        size_t block,
        size_t call,
        const Function &callee) {
    const auto &instructions = caller.getBasicBlocks()[block]->instructions;
    if (call >= instructions.size() || instructions[call].op != Op::Call) {
        return std::nullopt;
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
        return std::nullopt;
    }

    CallFrame site;
    site.block = block;
    site.frameStart = frameStart;
    site.call = call;
    site.frameEnd = call + 1;
    nesting = 0;
    for (size_t index = frameStart + 1; index < call; ++index) {
        if (instructions[index].op == Op::InStack) {
            ++nesting;
        } else if (instructions[index].op == Op::OutStack) {
            if (nesting == 0) {
                return std::nullopt;
            }
            --nesting;
        } else if (nesting == 0
                   && (instructions[index].op == Op::PushParam
                       || instructions[index].op == Op::PushAddressParam)) {
            site.pushes.push_back(index);
            site.arguments.push_back(instructions[index].arg1.get());
        }
    }
    if (nesting != 0 || site.pushes.size() != callee.getParams().size()) {
        return std::nullopt;
    }

    site.resultMove = instructions.size();
    if (callee.getReturnType() != Type::Void) {
        const size_t possibleResult = call + 2;
        if (possibleResult < instructions.size()
            && instructions[possibleResult].op == Op::NewMove) {
            const auto *physical = dynamic_cast<const Temp *>(
                    instructions[possibleResult].arg1.get());
            const auto *result = dynamic_cast<const Temp *>(
                    instructions[possibleResult].res.get());
            if (physical && physical->id == -2 && result) {
                site.resultMove = possibleResult;
                site.result = result;
            }
        }
    }
    return site;
}

std::unordered_map<int, int> useCounts(const Function &function) {
    std::unordered_map<int, int> uses;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            for (int used: usedTemps(inst)) {
                ++uses[used];
            }
        }
    }
    return uses;
}

void removeCallFrame(
        Function &caller,
        const CallFrame &site,
        std::optional<int> constantReturn,
        const Temp *replacement) {
    auto &instructions = caller.getMutableBasicBlocks()[site.block]->instructions;
    std::unordered_set<size_t> removed(site.pushes.begin(), site.pushes.end());
    removed.insert(site.frameStart);
    removed.insert(site.call);
    removed.insert(site.frameEnd);

    std::vector<Inst> kept;
    kept.reserve(instructions.size());
    for (size_t index = 0; index < instructions.size(); ++index) {
        if (removed.count(index) != 0) {
            continue;
        }
        if (index == site.resultMove) {
            if (!site.result) {
                continue;
            }
            if (constantReturn) {
                kept.emplace_back(
                        Op::LoadImd,
                        site.result->clone(),
                        std::make_unique<ConstVal>(*constantReturn, site.result->type),
                        nullptr);
            } else if (replacement) {
                kept.emplace_back(
                        Op::NewMove,
                        site.result->clone(),
                        replacement->clone(),
                        nullptr);
            }
            continue;
        }
        kept.push_back(std::move(instructions[index]));
    }
    instructions = std::move(kept);
}

std::string callKey(const std::string &callee, const CallFrame &site) {
    std::string key = callee;
    for (const Element *argument: site.arguments) {
        key += "|" + argument->valueKey();
    }
    return key;
}

bool simplifyCallsInFunction(
        Function &caller,
        const std::unordered_map<std::string, FunctionSummary> &summaries) {
    const auto uses = useCounts(caller);
    const auto &blocks = caller.getBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        std::unordered_map<std::string, Temp> availableCalls;
        for (size_t index = 0; index < blocks[block]->instructions.size(); ++index) {
            const Inst &inst = blocks[block]->instructions[index];
            if (inst.op != Op::Call) {
                if (directlyObservable(inst)) {
                    availableCalls.clear();
                }
                continue;
            }
            const auto *label = asLabel(inst.arg1);
            auto summary = label ? summaries.find(label->nameAndId) : summaries.end();
            if (!label || summary == summaries.end()) {
                availableCalls.clear();
                continue;
            }
            const auto site = callFrame(
                    caller, block, index, *summary->second.function);
            if (!site) {
                availableCalls.clear();
                continue;
            }

            if (summary->second.sideEffectFree && summary->second.terminating
                && summary->second.constantReturn) {
                removeCallFrame(
                        caller, *site, summary->second.constantReturn, nullptr);
                return true;
            }
            const bool resultUnused = !site->result
                                      || uses.find(site->result->id) == uses.end();
            if (summary->second.sideEffectFree && resultUnused) {
                removeCallFrame(caller, *site, std::nullopt, nullptr);
                return true;
            }
            if (!summary->second.sideEffectFree || summary->second.readsExternalMemory
                || !site->result) {
                availableCalls.clear();
                continue;
            }

            const std::string key = callKey(label->nameAndId, *site);
            auto previous = availableCalls.find(key);
            if (previous != availableCalls.end()) {
                removeCallFrame(caller, *site, std::nullopt, &previous->second);
                return true;
            }
            availableCalls.emplace(key, *site->result);
        }
    }
    return false;
}

} // namespace

bool optimizeInterproceduralCalls(Module &module) {
    const auto summaries = summarize(module);
    if (simplifyCallsInFunction(module.getMutableMainFunction(), summaries)) {
        return true;
    }
    for (auto &function: module.getMutableFunctions()) {
        if (simplifyCallsInFunction(*function, summaries)) {
            return true;
        }
    }
    return false;
}

} // namespace IR
