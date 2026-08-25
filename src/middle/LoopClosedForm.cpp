#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

struct LoopControl {
    size_t header{};
    size_t preheader{};
    size_t latch{};
    size_t body{};
    size_t exit{};
    std::unordered_set<size_t> blocks;
    const Temp *induction{};
    const Element *initial{};
    const Element *bound{};
    int step{};
    Op comparison{Op::Empty};
    bool continuesWhenComparisonIsTrue{};
};

struct Reduction {
    const Temp *accumulator{};
    const Element *initial{};
    const Element *contribution{};
    int sign{1};
    std::optional<TempDefinition> contributionToHoist;
};

std::unordered_map<std::string, size_t> blockIndices(const Function &function) {
    std::unordered_map<std::string, size_t> result;
    const auto &blocks = function.getBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        result.emplace(blocks[block]->label.nameAndId, block);
    }
    return result;
}

Op swapComparison(Op comparison) {
    switch (comparison) {
        case Op::Leq:
            return Op::Geq;
        case Op::Lss:
            return Op::Gre;
        case Op::Geq:
            return Op::Leq;
        case Op::Gre:
            return Op::Lss;
        default:
            return comparison;
    }
}

Op negateComparison(Op comparison) {
    switch (comparison) {
        case Op::Leq:
            return Op::Gre;
        case Op::Lss:
            return Op::Geq;
        case Op::Geq:
            return Op::Lss;
        case Op::Gre:
            return Op::Leq;
        case Op::Eql:
            return Op::Neq;
        case Op::Neq:
            return Op::Eql;
        default:
            return Op::Empty;
    }
}

std::optional<LoopControl> analyzeLoopControl(
        const Function &function,
        const ControlFlowGraph &cfg,
        size_t header,
        const std::unordered_set<size_t> &loop,
        const std::unordered_map<std::string, size_t> &labels,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants) {
    const auto &blocks = function.getBasicBlocks();
    std::vector<size_t> outsidePredecessors;
    std::vector<size_t> latches;
    for (size_t predecessor: cfg.predecessors[header]) {
        (loop.count(predecessor) == 0 ? outsidePredecessors : latches)
                .push_back(predecessor);
    }
    if (outsidePredecessors.size() != 1 || latches.size() != 1
        || cfg.successors[outsidePredecessors.front()].size() != 1
        || cfg.successors[header].size() != 2) {
        return std::nullopt;
    }

    size_t body = ControlFlowGraph::NoBlock;
    size_t exit = ControlFlowGraph::NoBlock;
    for (size_t successor: cfg.successors[header]) {
        (loop.count(successor) != 0 ? body : exit) = successor;
    }
    if (body == ControlFlowGraph::NoBlock || exit == ControlFlowGraph::NoBlock) {
        return std::nullopt;
    }
    for (size_t block: loop) {
        for (size_t successor: cfg.successors[block]) {
            if (loop.count(successor) == 0
                && (block != header || successor != exit)) {
                return std::nullopt;
            }
        }
    }

    const Inst *conditional = nullptr;
    const Inst *fallthrough = nullptr;
    for (const auto &inst: blocks[header]->instructions) {
        if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
            conditional = &inst;
        } else if (inst.op == Op::Br) {
            fallthrough = &inst;
        }
    }
    const auto *taken = conditional ? asLabel(conditional->arg2) : nullptr;
    const auto *other = fallthrough ? asLabel(fallthrough->arg1) : nullptr;
    const auto takenBlock = taken ? labels.find(taken->nameAndId) : labels.end();
    const auto otherBlock = other ? labels.find(other->nameAndId) : labels.end();
    if (!conditional || !taken || !other || takenBlock == labels.end()
        || otherBlock == labels.end()) {
        return std::nullopt;
    }
    const size_t trueBlock = conditional->op == Op::Bif1
                                     ? takenBlock->second
                                     : otherBlock->second;
    const size_t falseBlock = conditional->op == Op::Bif1
                                      ? otherBlock->second
                                      : takenBlock->second;
    const bool continuesWhenTrue = trueBlock == body && falseBlock == exit;
    const bool continuesWhenFalse = falseBlock == body && trueBlock == exit;
    if (!continuesWhenTrue && !continuesWhenFalse) {
        return std::nullopt;
    }

    const auto *condition = dynamic_cast<const Temp *>(conditional->arg1.get());
    auto conditionDefinition = condition ? defs.find(condition->id) : defs.end();
    if (!condition || conditionDefinition == defs.end()
        || conditionDefinition->second.block != header) {
        return std::nullopt;
    }
    const Inst &comparison = blocks[header]->instructions[conditionDefinition->second.instruction];
    if (comparison.op != Op::Leq && comparison.op != Op::Lss
        && comparison.op != Op::Geq && comparison.op != Op::Gre) {
        return std::nullopt;
    }

    for (const auto &phi: blocks[header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *induction = dynamic_cast<const Temp *>(phi.res.get());
        if (!induction || induction->type != Type::Int || phi.phiIncoming.size() != 2
            || (!sameTemp(comparison.arg1.get(), induction->id)
                && !sameTemp(comparison.arg2.get(), induction->id))) {
            continue;
        }

        const Element *initial = nullptr;
        const Temp *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor
                == blocks[outsidePredecessors.front()]->label.nameAndId) {
                initial = incoming.value.get();
            } else if (incoming.predecessor == blocks[latches.front()]->label.nameAndId) {
                backedge = dynamic_cast<const Temp *>(incoming.value.get());
            }
        }
        auto updateDefinition = backedge ? defs.find(backedge->id) : defs.end();
        if (!initial || !backedge || updateDefinition == defs.end()
            || loop.count(updateDefinition->second.block) == 0) {
            continue;
        }
        const Inst &update = blocks[updateDefinition->second.block]->instructions[updateDefinition->second.instruction];
        std::optional<int> step;
        if (update.op == Op::Add) {
            if (sameTemp(update.arg1.get(), induction->id)) {
                step = constantValue(update.arg2.get(), knownConstants);
            } else if (sameTemp(update.arg2.get(), induction->id)) {
                step = constantValue(update.arg1.get(), knownConstants);
            }
        } else if (update.op == Op::Sub
                   && sameTemp(update.arg1.get(), induction->id)) {
            auto value = constantValue(update.arg2.get(), knownConstants);
            if (value) {
                step = static_cast<int32_t>(0U - static_cast<uint32_t>(*value));
            }
        }
        if (!step || *step == 0) {
            continue;
        }

        Op normalizedComparison = comparison.op;
        const Element *bound = comparison.arg2.get();
        if (sameTemp(comparison.arg2.get(), induction->id)) {
            normalizedComparison = swapComparison(normalizedComparison);
            bound = comparison.arg1.get();
        }
        if (!continuesWhenTrue) {
            normalizedComparison = negateComparison(normalizedComparison);
        }
        if (normalizedComparison == Op::Empty) {
            return std::nullopt;
        }
        return LoopControl{header,
                           outsidePredecessors.front(),
                           latches.front(),
                           body,
                           exit,
                           loop,
                           induction,
                           initial,
                           bound,
                           *step,
                           normalizedComparison,
                           continuesWhenTrue};
    }
    return std::nullopt;
}

std::optional<int> fixedIterationCount(
        const LoopControl &loop,
        const std::unordered_map<int, int> &knownConstants) {
    const auto initial = constantValue(loop.initial, knownConstants);
    const auto bound = constantValue(loop.bound, knownConstants);
    if (!initial || !bound) {
        return std::nullopt;
    }

    const std::int64_t start = *initial;
    const std::int64_t limit = *bound;
    const std::int64_t step = loop.step;
    std::int64_t count = 0;
    if (step > 0 && loop.comparison == Op::Leq) {
        count = start > limit ? 0 : (limit - start) / step + 1;
    } else if (step > 0 && loop.comparison == Op::Lss) {
        count = start >= limit ? 0 : (limit - start - 1) / step + 1;
    } else if (step < 0 && loop.comparison == Op::Geq) {
        count = start < limit ? 0 : (start - limit) / -step + 1;
    } else if (step < 0 && loop.comparison == Op::Gre) {
        count = start <= limit ? 0 : (start - limit - 1) / -step + 1;
    } else {
        return std::nullopt;
    }
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    const std::int64_t finalValue = start + count * step;
    if (finalValue < std::numeric_limits<int>::min()
        || finalValue > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(count);
}

bool loopHasObservableEffects(
        const Function &function,
        const std::unordered_set<size_t> &loop) {
    const auto &blocks = function.getBasicBlocks();
    for (size_t block: loop) {
        for (const auto &inst: blocks[block]->instructions) {
            if (!inst.mayHaveSideEffects() || inst.isScopeMarker()
                || inst.op == Op::Br || inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                continue;
            }
            return true;
        }
    }
    return false;
}

std::unordered_set<int> externallyUsedLoopValues(
        const Function &function,
        const std::unordered_set<size_t> &loop) {
    std::unordered_set<int> loopDefinitions;
    const auto &blocks = function.getBasicBlocks();
    for (size_t block: loop) {
        for (const auto &inst: blocks[block]->instructions) {
            auto definition = definedTemp(inst);
            if (definition) {
                loopDefinitions.insert(*definition);
            }
        }
    }

    std::unordered_set<int> result;
    for (size_t block = 0; block < blocks.size(); ++block) {
        if (loop.count(block) != 0) {
            continue;
        }
        for (const auto &inst: blocks[block]->instructions) {
            for (int used: usedTemps(inst)) {
                if (loopDefinitions.count(used) != 0) {
                    result.insert(used);
                }
            }
        }
    }
    return result;
}

std::optional<Reduction> findReduction(
        const Function &function,
        const LoopControl &loop,
        const TempDefinitions &defs) {
    const auto &blocks = function.getBasicBlocks();
    for (const auto &phi: blocks[loop.header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *accumulator = dynamic_cast<const Temp *>(phi.res.get());
        if (!accumulator || accumulator->id == loop.induction->id
            || accumulator->type != Type::Int || phi.phiIncoming.size() != 2) {
            continue;
        }

        const Element *initial = nullptr;
        const Temp *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor
                == blocks[loop.preheader]->label.nameAndId) {
                initial = incoming.value.get();
            } else if (incoming.predecessor == blocks[loop.latch]->label.nameAndId) {
                backedge = dynamic_cast<const Temp *>(incoming.value.get());
            }
        }
        auto updateDefinition = backedge ? defs.find(backedge->id) : defs.end();
        if (!initial || !backedge || updateDefinition == defs.end()
            || loop.blocks.count(updateDefinition->second.block) == 0) {
            continue;
        }
        const Inst &update = blocks[updateDefinition->second.block]->instructions[updateDefinition->second.instruction];
        const Element *contribution = nullptr;
        int sign = 1;
        if (update.op == Op::Add) {
            if (sameTemp(update.arg1.get(), accumulator->id)) {
                contribution = update.arg2.get();
            } else if (sameTemp(update.arg2.get(), accumulator->id)) {
                contribution = update.arg1.get();
            }
        } else if (update.op == Op::Sub
                   && sameTemp(update.arg1.get(), accumulator->id)) {
            contribution = update.arg2.get();
            sign = -1;
        }
        if (!contribution) {
            continue;
        }

        std::optional<TempDefinition> hoist;
        if (const auto *temp = dynamic_cast<const Temp *>(contribution)) {
            auto definition = defs.find(temp->id);
            if (definition != defs.end() && loop.blocks.count(definition->second.block) != 0) {
                const Inst &producer = blocks[definition->second.block]->instructions[definition->second.instruction];
                if (producer.op != Op::Load && producer.op != Op::LoadDynamic
                    && producer.op != Op::LoadPtr) {
                    continue;
                }
                bool invariantAddress = true;
                for (int used: usedTemps(producer)) {
                    auto usedDefinition = defs.find(used);
                    if (usedDefinition != defs.end()
                        && loop.blocks.count(usedDefinition->second.block) != 0) {
                        invariantAddress = false;
                        break;
                    }
                }
                if (!invariantAddress) {
                    continue;
                }
                hoist = definition->second;
            }
        }
        return Reduction{accumulator, initial, contribution, sign, hoist};
    }
    return std::nullopt;
}

bool exitStartsWithPhi(const Function &function, size_t exit) {
    const auto &instructions = function.getBasicBlocks()[exit]->instructions;
    return !instructions.empty() && instructions.front().op == Op::Phi;
}

std::pair<size_t, size_t> branchTargets(
        const Function &function,
        size_t block,
        const std::unordered_map<std::string, size_t> &labels) {
    const Inst *conditional = nullptr;
    const Inst *fallthrough = nullptr;
    for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
        if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
            conditional = &inst;
        } else if (inst.op == Op::Br) {
            fallthrough = &inst;
        }
    }
    const auto *taken = conditional ? asLabel(conditional->arg2) : nullptr;
    const auto *other = fallthrough ? asLabel(fallthrough->arg1) : nullptr;
    auto takenBlock = taken ? labels.find(taken->nameAndId) : labels.end();
    auto otherBlock = other ? labels.find(other->nameAndId) : labels.end();
    if (!conditional || takenBlock == labels.end() || otherBlock == labels.end()) {
        return {ControlFlowGraph::NoBlock, ControlFlowGraph::NoBlock};
    }
    return conditional->op == Op::Bif1
                   ? std::make_pair(takenBlock->second, otherBlock->second)
                   : std::make_pair(otherBlock->second, takenBlock->second);
}

bool trueBranchReaches(
        const Function &function,
        const ControlFlowGraph &cfg,
        const std::unordered_map<std::string, size_t> &labels,
        size_t comparisonBlock,
        size_t destination) {
    const auto [trueBlock, falseBlock] = branchTargets(
            function, comparisonBlock, labels);
    return trueBlock != ControlFlowGraph::NoBlock
           && cfg.dominates(trueBlock, destination)
           && !cfg.dominates(falseBlock, destination);
}

bool redirectPreheaderToExit(Function &function, const LoopControl &loop);

bool collapseWholeArrayZeroFill(
        Function &function,
        const LoopControl &loop,
        int iterations,
        const std::unordered_map<int, int> &knownConstants) {
    if (iterations < 32 || loop.step != 1
        || loop.comparison != Op::Lss
        || !loop.continuesWhenComparisonIsTrue
        || constantValue(loop.initial, knownConstants) != 0
        || exitStartsWithPhi(function, loop.exit)
        || !externallyUsedLoopValues(function, loop.blocks).empty()) {
        return false;
    }

    const Inst *zeroStore = nullptr;
    const Var *base = nullptr;
    for (size_t block: loop.blocks) {
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            if (inst.op == Op::Store || inst.op == Op::StoreDynamic) {
                if (zeroStore) {
                    return false;
                }
                zeroStore = &inst;
                base = dynamic_cast<const Var *>(inst.arg1.get());
                continue;
            }
            if (inst.mayHaveSideEffects() && !inst.isScopeMarker()
                && inst.op != Op::Br && inst.op != Op::Bif0
                && inst.op != Op::Bif1) {
                return false;
            }
        }
    }
    if (!zeroStore || zeroStore->op != Op::StoreDynamic || !base
        || base->depth != 0 || base->storesAddress
        || ptrToValue(base->type) != Type::Char
        || !sameTemp(zeroStore->arg2.get(), loop.induction->id)
        || constantValue(zeroStore->res.get(), knownConstants) != 0) {
        return false;
    }

    std::int64_t elements = 1;
    for (int dimension: base->dims) {
        if (dimension <= 0
            || elements > std::numeric_limits<int>::max() / dimension) {
            return false;
        }
        elements *= dimension;
    }
    if (base->dims.empty() || elements != iterations) {
        return false;
    }

    auto &preheader = function.getMutableBasicBlocks()[loop.preheader]->instructions;
    auto terminator = std::find_if(
            preheader.begin(), preheader.end(),
            [](const Inst &inst) { return inst.isTerminator(); });
    preheader.insert(
            terminator,
            Inst(Op::MemZero,
                 nullptr,
                 base->clone(),
                 std::make_unique<ConstVal>(iterations, Type::Int)));
    return redirectPreheaderToExit(function, loop);
}

bool versionBitReconstructionLoop(
        Function &function,
        const ControlFlowGraph &cfg,
        const LoopControl &loop,
        int iterations,
        const std::unordered_map<std::string, size_t> &labels,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants) {
    if (iterations != 16 || loop.step != 1
        || constantValue(loop.initial, knownConstants) != 0
        || loopHasObservableEffects(function, loop.blocks)
        || exitStartsWithPhi(function, loop.exit)) {
        return false;
    }

    const auto &blocks = function.getBasicBlocks();
    struct StateValue {
        const Temp *phi{};
        const Element *initial{};
    };
    std::vector<StateValue> states;
    const Temp *place = nullptr;
    const Temp *result = nullptr;
    const Element *resultInitial = nullptr;
    const Inst *resultMerge = nullptr;
    for (const auto &phi: blocks[loop.header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *value = dynamic_cast<const Temp *>(phi.res.get());
        if (!value || value->id == loop.induction->id || phi.phiIncoming.size() != 2) {
            continue;
        }
        const Element *initial = nullptr;
        const Temp *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor == blocks[loop.preheader]->label.nameAndId) {
                initial = incoming.value.get();
            } else if (incoming.predecessor == blocks[loop.latch]->label.nameAndId) {
                backedge = dynamic_cast<const Temp *>(incoming.value.get());
            }
        }
        auto backedgeDefinition = backedge ? defs.find(backedge->id) : defs.end();
        if (!initial || !backedge || backedgeDefinition == defs.end()) {
            continue;
        }
        const Inst &update = blocks[backedgeDefinition->second.block]
                                     ->instructions[backedgeDefinition->second.instruction];
        if (update.op == Op::Div && sameTemp(update.arg1.get(), value->id)
            && constantValue(update.arg2.get(), knownConstants) == 2) {
            states.push_back({value, initial});
        } else if ((update.op == Op::Mul || update.op == Op::MulImd)
                   && ((sameTemp(update.arg1.get(), value->id)
                        && constantValue(update.arg2.get(), knownConstants) == 2)
                       || (sameTemp(update.arg2.get(), value->id)
                           && constantValue(update.arg1.get(), knownConstants) == 2))
                   && constantValue(initial, knownConstants) == 1) {
            place = value;
        } else if (update.op == Op::Phi
                   && constantValue(initial, knownConstants) == 0) {
            result = value;
            resultInitial = initial;
            resultMerge = &update;
        }
    }
    if (states.size() != 2 || !place || !result || !resultInitial || !resultMerge) {
        return false;
    }

    const Inst *conditionalAdd = nullptr;
    size_t addBlock = ControlFlowGraph::NoBlock;
    for (const auto &incoming: resultMerge->phiIncoming) {
        if (sameTemp(incoming.value.get(), result->id)) {
            continue;
        }
        const auto *value = dynamic_cast<const Temp *>(incoming.value.get());
        auto definition = value ? defs.find(value->id) : defs.end();
        if (!value || definition == defs.end()) {
            return false;
        }
        const Inst &candidate = blocks[definition->second.block]
                                        ->instructions[definition->second.instruction];
        const bool addsPlace = candidate.op == Op::Add
                               && ((sameTemp(candidate.arg1.get(), result->id)
                                    && sameTemp(candidate.arg2.get(), place->id))
                                   || (sameTemp(candidate.arg2.get(), result->id)
                                       && sameTemp(candidate.arg1.get(), place->id)));
        if (!addsPlace || conditionalAdd) {
            return false;
        }
        conditionalAdd = &candidate;
        addBlock = definition->second.block;
    }
    if (!conditionalAdd || addBlock == ControlFlowGraph::NoBlock) {
        return false;
    }

    std::unordered_map<int, int> remainderByState;
    for (size_t block: loop.blocks) {
        for (const auto &inst: blocks[block]->instructions) {
            const auto *remainder = dynamic_cast<const Temp *>(inst.res.get());
            if (inst.op != Op::Mod || !remainder
                || constantValue(inst.arg2.get(), knownConstants) != 2) {
                continue;
            }
            for (const auto &state: states) {
                if (sameTemp(inst.arg1.get(), state.phi->id)) {
                    remainderByState[state.phi->id] = remainder->id;
                }
            }
        }
    }
    if (remainderByState.size() != 2) {
        return false;
    }
    const int firstRemainder = remainderByState.at(states[0].phi->id);
    const int secondRemainder = remainderByState.at(states[1].phi->id);

    Op bitOperation = Op::Empty;
    std::unordered_set<int> andComparisons;
    for (size_t block: loop.blocks) {
        for (const auto &inst: blocks[block]->instructions) {
            if (inst.op == Op::Neq
                && ((sameTemp(inst.arg1.get(), firstRemainder)
                     && sameTemp(inst.arg2.get(), secondRemainder))
                    || (sameTemp(inst.arg2.get(), firstRemainder)
                        && sameTemp(inst.arg1.get(), secondRemainder)))
                && trueBranchReaches(function, cfg, labels, block, addBlock)) {
                bitOperation = Op::Xor;
            }
            if (inst.op != Op::Eql) {
                continue;
            }
            for (int remainder: {firstRemainder, secondRemainder}) {
                if (((sameTemp(inst.arg1.get(), remainder)
                      && constantValue(inst.arg2.get(), knownConstants) == 1)
                     || (sameTemp(inst.arg2.get(), remainder)
                         && constantValue(inst.arg1.get(), knownConstants) == 1))
                    && trueBranchReaches(function, cfg, labels, block, addBlock)) {
                    andComparisons.insert(remainder);
                }
            }
        }
    }
    if (bitOperation == Op::Empty && andComparisons.size() == 2) {
        bitOperation = Op::And;
    }
    if (bitOperation == Op::Empty) {
        return false;
    }

    const auto externalUses = externallyUsedLoopValues(function, loop.blocks);
    for (int used: externalUses) {
        if (used != result->id) {
            return false;
        }
    }

    auto &mutableBlocks = function.getMutableBasicBlocks();
    const Temp closedResult(nextTempId(function), Type::Int);
    auto &preheader = mutableBlocks[loop.preheader]->instructions;
    auto terminator = std::find_if(
            preheader.begin(), preheader.end(),
            [](const Inst &inst) { return inst.isTerminator(); });
    preheader.insert(
            terminator,
            Inst(bitOperation == Op::Xor ? Op::XorLimb : Op::AndLimb,
                 closedResult.clone(),
                 states[0].initial->clone(),
                 states[1].initial->clone()));

    for (size_t block = 0; block < mutableBlocks.size(); ++block) {
        if (loop.blocks.count(block) != 0) {
            continue;
        }
        for (auto &inst: mutableBlocks[block]->instructions) {
            for (const auto &operand: inst.operands()) {
                if (operand.role == OperandRole::Definition
                    || !sameTemp(operand.value, result->id)) {
                    continue;
                }
                inst.setOperand(operand.slot, closedResult.clone());
            }
        }
    }
    return redirectPreheaderToExit(function, loop);
}

bool redirectPreheaderToExit(Function &function, const LoopControl &loop) {
    auto &instructions = function.getMutableBasicBlocks()[loop.preheader]->instructions;
    const std::string &headerLabel = function.getBasicBlocks()[loop.header]->label.nameAndId;
    for (auto reverse = instructions.rbegin(); reverse != instructions.rend(); ++reverse) {
        const auto *target = reverse->op == Op::Br ? asLabel(reverse->arg1) : nullptr;
        if (target && target->nameAndId == headerLabel) {
            reverse->arg1 = function.getBasicBlocks()[loop.exit]->label.clone();
            return true;
        }
    }
    return false;
}

bool collapseFixedReduction(
        Function &function,
        const LoopControl &loop,
        int iterations,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants) {
    if (loopHasObservableEffects(function, loop.blocks)
        || exitStartsWithPhi(function, loop.exit)) {
        return false;
    }

    const auto reduction = findReduction(function, loop, defs);
    const auto externalUses = externallyUsedLoopValues(function, loop.blocks);
    for (int used: externalUses) {
        if (used != loop.induction->id
            && (!reduction || used != reduction->accumulator->id)) {
            return false;
        }
    }
    if (!reduction && !externalUses.empty()) {
        return false;
    }

    auto &blocks = function.getMutableBasicBlocks();
    std::vector<Inst> setup;
    if (reduction && reduction->contributionToHoist) {
        const TempDefinition location = *reduction->contributionToHoist;
        auto &instructions = blocks[location.block]->instructions;
        setup.push_back(std::move(instructions[location.instruction]));
        instructions.erase(instructions.begin()
                           + static_cast<long>(location.instruction));
    }

    int id = nextTempId(function);
    if (reduction) {
        std::unique_ptr<Element> finalValue;
        if (iterations == 0) {
            finalValue = reduction->initial->clone();
        } else {
            const int factor = reduction->sign == 1 ? iterations : -iterations;
            const auto contributionConstant = constantValue(
                    reduction->contribution, knownConstants);
            const auto initialConstant = constantValue(
                    reduction->initial, knownConstants);
            if (contributionConstant && initialConstant) {
                const Temp result(id++, Type::Int);
                setup.emplace_back(
                        Op::LoadImd,
                        result.clone(),
                        std::make_unique<ConstVal>(
                                wrappingAdd(*initialConstant,
                                            wrappingMultiply(*contributionConstant, factor)),
                                Type::Int),
                        nullptr);
                finalValue = result.clone();
            } else {
                const Temp scaled(id++, Type::Int);
                setup.emplace_back(
                        Op::MulImd,
                        scaled.clone(),
                        reduction->contribution->clone(),
                        std::make_unique<ConstVal>(factor, Type::Int));
                if (initialConstant && *initialConstant == 0) {
                    finalValue = scaled.clone();
                } else {
                    const Temp result(id++, Type::Int);
                    setup.emplace_back(
                            Op::Add,
                            result.clone(),
                            reduction->initial->clone(),
                            scaled.clone());
                    finalValue = result.clone();
                }
            }
        }
        function.replaceAllUsesWith(*reduction->accumulator, *finalValue);
    }

    if (externalUses.count(loop.induction->id) != 0) {
        const auto initial = constantValue(loop.initial, knownConstants);
        const Temp finalInduction(id++, Type::Int);
        if (initial) {
            setup.emplace_back(
                    Op::LoadImd,
                    finalInduction.clone(),
                    std::make_unique<ConstVal>(
                            wrappingAdd(*initial, wrappingMultiply(iterations, loop.step)),
                            Type::Int),
                    nullptr);
        } else {
            setup.emplace_back(
                    Op::Add,
                    finalInduction.clone(),
                    loop.initial->clone(),
                    std::make_unique<ConstVal>(
                            wrappingMultiply(iterations, loop.step), Type::Int));
        }
        function.replaceAllUsesWith(*loop.induction, finalInduction);
    }

    auto &preheader = blocks[loop.preheader]->instructions;
    auto terminator = std::find_if(
            preheader.begin(), preheader.end(),
            [](const Inst &inst) { return inst.isTerminator(); });
    preheader.insert(terminator,
                     std::make_move_iterator(setup.begin()),
                     std::make_move_iterator(setup.end()));
    return redirectPreheaderToExit(function, loop);
}

std::optional<int> phiIncomingDelta(
        const Function &function,
        const PhiIncoming &incoming,
        int accumulator,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants) {
    if (sameTemp(incoming.value.get(), accumulator)) {
        return 0;
    }
    const auto *value = dynamic_cast<const Temp *>(incoming.value.get());
    auto definition = value ? defs.find(value->id) : defs.end();
    if (!value || definition == defs.end()) {
        return std::nullopt;
    }
    const Inst &update = function.getBasicBlocks()[definition->second.block]
                                 ->instructions[definition->second.instruction];
    if (update.op != Op::Add) {
        return std::nullopt;
    }
    if (sameTemp(update.arg1.get(), accumulator)) {
        return constantValue(update.arg2.get(), knownConstants);
    }
    if (sameTemp(update.arg2.get(), accumulator)) {
        return constantValue(update.arg1.get(), knownConstants);
    }
    return std::nullopt;
}

bool branchProvesResidue(
        const Function &function,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants,
        int remainder,
        int residue,
        const std::string &target) {
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &branch: block->instructions) {
            if (branch.op != Op::Bif1) {
                continue;
            }
            const auto *label = asLabel(branch.arg2);
            const auto *condition = dynamic_cast<const Temp *>(branch.arg1.get());
            auto definition = condition ? defs.find(condition->id) : defs.end();
            if (!label || label->nameAndId != target || definition == defs.end()) {
                continue;
            }
            const Inst &comparison = function.getBasicBlocks()[definition->second.block]
                                             ->instructions[definition->second.instruction];
            if (comparison.op != Op::Eql) {
                continue;
            }
            if ((sameTemp(comparison.arg1.get(), remainder)
                 && constantValue(comparison.arg2.get(), knownConstants) == residue)
                || (sameTemp(comparison.arg2.get(), remainder)
                    && constantValue(comparison.arg1.get(), knownConstants) == residue)) {
                return true;
            }
        }
    }
    return false;
}

bool zeroResidueTarget(
        const Function &function,
        const std::unordered_map<std::string, size_t> &labels,
        int remainder,
        const std::unordered_set<std::string> &zeroTargets) {
    for (const auto &block: function.getBasicBlocks()) {
        const Inst *conditional = nullptr;
        const Inst *fallthrough = nullptr;
        for (const auto &inst: block->instructions) {
            if ((inst.op == Op::Bif0 || inst.op == Op::Bif1)
                && sameTemp(inst.arg1.get(), remainder)) {
                conditional = &inst;
            } else if (inst.op == Op::Br) {
                fallthrough = &inst;
            }
        }
        const auto *taken = conditional ? asLabel(conditional->arg2) : nullptr;
        const auto *other = fallthrough ? asLabel(fallthrough->arg1) : nullptr;
        if (!taken || !other || labels.count(taken->nameAndId) == 0
            || labels.count(other->nameAndId) == 0) {
            continue;
        }
        const std::string &zeroTarget = conditional->op == Op::Bif0
                                                ? taken->nameAndId
                                                : other->nameAndId;
        if (zeroTargets.count(zeroTarget) != 0) {
            return true;
        }
    }
    return false;
}

bool collapsePeriodicRemainderSum(
        Function &function,
        const LoopControl &loop,
        const std::unordered_map<std::string, size_t> &labels,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &knownConstants) {
    if (!loop.continuesWhenComparisonIsTrue || loop.comparison != Op::Leq
        || loop.step != 1 || constantValue(loop.initial, knownConstants) != 1
        || constantValue(loop.bound, knownConstants)
        || loopHasObservableEffects(function, loop.blocks)
        || exitStartsWithPhi(function, loop.exit)) {
        return false;
    }

    const auto reduction = findReduction(function, loop, defs);
    if (reduction) {
        return false;
    }

    const auto &blocks = function.getBasicBlocks();
    const Temp *accumulator = nullptr;
    const Element *accumulatorInitial = nullptr;
    const Inst *merge = nullptr;
    for (const auto &phi: blocks[loop.header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *candidate = dynamic_cast<const Temp *>(phi.res.get());
        if (!candidate || candidate->id == loop.induction->id
            || candidate->type != Type::Int || phi.phiIncoming.size() != 2) {
            continue;
        }
        const Temp *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor == blocks[loop.preheader]->label.nameAndId) {
                accumulatorInitial = incoming.value.get();
            } else if (incoming.predecessor == blocks[loop.latch]->label.nameAndId) {
                backedge = dynamic_cast<const Temp *>(incoming.value.get());
            }
        }
        auto backedgeDefinition = backedge ? defs.find(backedge->id) : defs.end();
        if (accumulatorInitial && backedge && backedgeDefinition != defs.end()) {
            const Inst &candidateMerge = blocks[backedgeDefinition->second.block]
                                                 ->instructions[backedgeDefinition->second.instruction];
            if (candidateMerge.op == Op::Phi) {
                accumulator = candidate;
                merge = &candidateMerge;
                break;
            }
        }
        accumulatorInitial = nullptr;
    }
    if (!accumulator || !accumulatorInitial || !merge) {
        return false;
    }

    const Temp *remainder = nullptr;
    int period = 0;
    for (size_t block: loop.blocks) {
        for (const auto &inst: blocks[block]->instructions) {
            if (inst.op != Op::Mod || !sameTemp(inst.arg1.get(), loop.induction->id)) {
                continue;
            }
            const auto candidatePeriod = constantValue(inst.arg2.get(), knownConstants);
            const auto *candidateRemainder = dynamic_cast<const Temp *>(inst.res.get());
            if (candidatePeriod && *candidatePeriod >= 2 && *candidatePeriod <= 1024
                && candidateRemainder) {
                remainder = candidateRemainder;
                period = *candidatePeriod;
                break;
            }
        }
    }
    if (!remainder) {
        return false;
    }

    std::unordered_map<int, std::string> residueTargets;
    std::unordered_set<std::string> zeroTargets;
    std::vector<bool> covered(static_cast<size_t>(period), false);
    for (const auto &incoming: merge->phiIncoming) {
        const auto delta = phiIncomingDelta(
                function, incoming, accumulator->id, defs, knownConstants);
        if (!delta || *delta < 0 || *delta >= period) {
            return false;
        }
        covered[static_cast<size_t>(*delta)] = true;
        if (*delta == 0) {
            zeroTargets.insert(incoming.predecessor);
        } else if (!residueTargets.emplace(*delta, incoming.predecessor).second) {
            return false;
        }
    }
    if (std::any_of(covered.begin(), covered.end(), [](bool value) { return !value; })
        || !zeroResidueTarget(function, labels, remainder->id, zeroTargets)) {
        return false;
    }
    for (int residue = 1; residue < period; ++residue) {
        auto target = residueTargets.find(residue);
        if (target == residueTargets.end()
            || !branchProvesResidue(function, defs, knownConstants,
                                    remainder->id, residue, target->second)) {
            return false;
        }
    }

    const auto externalUses = externallyUsedLoopValues(function, loop.blocks);
    for (int used: externalUses) {
        if (used != accumulator->id && used != loop.induction->id) {
            return false;
        }
    }

    auto &mutableBlocks = function.getMutableBasicBlocks();
    auto &fastPath = mutableBlocks[loop.body]->instructions;
    fastPath.clear();
    int id = nextTempId(function);
    const Temp one(id++, Type::Int);
    const Temp quotient(id++, Type::Int);
    const Temp quotientProduct(id++, Type::Int);
    const Temp tailCount(id++, Type::Int);
    const Temp tailPlusOne(id++, Type::Int);
    const Temp tailProduct(id++, Type::Int);
    const Temp tailSum(id++, Type::Int);
    const Temp cycleSum(id++, Type::Int);
    const Temp delta(id++, Type::Int);
    const Temp finalValue(id++, Type::Int);
    const Temp mergedValue(id++, Type::Int);
    const int oneCycle = period * (period - 1) / 2;

    fastPath.emplace_back(
            Op::LoadImd, one.clone(),
            std::make_unique<ConstVal>(1, Type::Int), nullptr);
    fastPath.emplace_back(
            Op::Div, quotient.clone(), loop.bound->clone(),
            std::make_unique<ConstVal>(period, Type::Int));
    fastPath.emplace_back(
            Op::MulImd, quotientProduct.clone(), quotient.clone(),
            std::make_unique<ConstVal>(period, Type::Int));
    fastPath.emplace_back(
            Op::Sub, tailCount.clone(), loop.bound->clone(), quotientProduct.clone());
    fastPath.emplace_back(
            Op::Add, tailPlusOne.clone(), tailCount.clone(), one.clone());
    fastPath.emplace_back(
            Op::Mul, tailProduct.clone(), tailCount.clone(), tailPlusOne.clone());
    fastPath.emplace_back(
            Op::Div, tailSum.clone(), tailProduct.clone(),
            std::make_unique<ConstVal>(2, Type::Int));
    fastPath.emplace_back(
            Op::MulImd, cycleSum.clone(), quotient.clone(),
            std::make_unique<ConstVal>(oneCycle, Type::Int));
    fastPath.emplace_back(
            Op::Add, delta.clone(), cycleSum.clone(), tailSum.clone());
    fastPath.emplace_back(
            Op::Add, finalValue.clone(), accumulatorInitial->clone(), delta.clone());
    fastPath.emplace_back(
            Op::Br, nullptr, mutableBlocks[loop.exit]->label.clone(), nullptr);

    function.replaceAllUsesWith(*accumulator, mergedValue);
    Inst resultPhi(Op::Phi, mergedValue.clone(), nullptr, nullptr);
    resultPhi.addPhiIncoming(
            mutableBlocks[loop.header]->label.nameAndId,
            accumulatorInitial->clone());
    resultPhi.addPhiIncoming(
            mutableBlocks[loop.body]->label.nameAndId,
            finalValue.clone());
    mutableBlocks[loop.exit]->instructions.insert(
            mutableBlocks[loop.exit]->instructions.begin(), std::move(resultPhi));
    return true;
}

std::unique_ptr<Element> cloneUnrolledElement(
        const Element *element,
        const std::unordered_map<int, std::unique_ptr<Element>> &state,
        const std::unordered_map<int, Temp> &iterationValues) {
    if (!element) {
        return nullptr;
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    if (!temp || temp->id < 0) {
        return element->clone();
    }
    auto iterationValue = iterationValues.find(temp->id);
    if (iterationValue != iterationValues.end()) {
        return iterationValue->second.clone();
    }
    auto stateValue = state.find(temp->id);
    return stateValue == state.end() ? temp->clone()
                                     : stateValue->second->clone();
}

bool unrollSmallSingleBlockLoop(Function &function,
                                const LoopControl &loop,
                                int iterations) {
    if (iterations < 0 || iterations > 8
        || loop.blocks.count(loop.header) == 0
        || loop.blocks.count(loop.body) == 0) {
        return false;
    }
    const auto &blocks = function.getBasicBlocks();
    const auto cfg = buildControlFlowGraph(function);
    std::vector<size_t> bodyOrder;
    std::unordered_set<size_t> visited;
    size_t current = loop.body;
    while (current != loop.header) {
        if (loop.blocks.count(current) == 0 || !visited.insert(current).second) {
            return false;
        }
        bodyOrder.push_back(current);
        for (const auto &inst: blocks[current]->instructions) {
            if (inst.op == Op::Phi || inst.op == Op::Alloca
                || inst.op == Op::Bif0 || inst.op == Op::Bif1
                || inst.op == Op::Ret || inst.op == Op::RetMain) {
                return false;
            }
        }
        if (cfg.successors[current].size() != 1) {
            return false;
        }
        current = cfg.successors[current].front();
    }
    if (visited.size() + 1 != loop.blocks.size()
        || bodyOrder.empty() || bodyOrder.back() != loop.latch) {
        return false;
    }

    struct PhiState {
        int id{};
        Type type{Type::Int};
        const Element *initial{};
        const Element *backedge{};
    };
    std::vector<PhiState> phis;
    for (const auto &phi: blocks[loop.header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *result = dynamic_cast<const Temp *>(phi.res.get());
        const Element *initial = nullptr;
        const Element *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor == blocks[loop.preheader]->label.nameAndId) {
                initial = incoming.value.get();
            } else if (incoming.predecessor == blocks[loop.latch]->label.nameAndId) {
                backedge = incoming.value.get();
            }
        }
        if (!result || !initial || !backedge) {
            return false;
        }
        phis.push_back({result->id, result->type, initial, backedge});
    }
    if (phis.empty()) {
        return false;
    }

    std::unordered_map<int, std::unique_ptr<Element>> state;
    for (const auto &phi: phis) {
        state.emplace(phi.id, phi.initial->clone());
    }
    int id = nextTempId(function);
    std::vector<Inst> expanded;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::unordered_map<int, Temp> iterationValues;
        for (size_t bodyBlock: bodyOrder) {
            for (const auto &inst: blocks[bodyBlock]->instructions) {
                if (inst.op == Op::Br || inst.isScopeMarker()) {
                    continue;
                }
                std::unique_ptr<Element> result;
                if (const auto *oldResult = dynamic_cast<const Temp *>(inst.res.get());
                    oldResult && oldResult->id >= 0 && inst.definesTemp()) {
                    const Temp replacement(id++, oldResult->type);
                    iterationValues.emplace(oldResult->id, replacement);
                    result = replacement.clone();
                } else {
                    result = cloneUnrolledElement(
                            inst.res.get(), state, iterationValues);
                }
                Inst clone(
                        inst.op,
                        std::move(result),
                        cloneUnrolledElement(inst.arg1.get(), state, iterationValues),
                        cloneUnrolledElement(inst.arg2.get(), state, iterationValues));
                expanded.push_back(std::move(clone));
            }
        }

        std::unordered_map<int, std::unique_ptr<Element>> nextState;
        for (const auto &phi: phis) {
            nextState.emplace(
                    phi.id,
                    cloneUnrolledElement(phi.backedge, state, iterationValues));
        }
        state = std::move(nextState);
    }

    auto &mutableBlocks = function.getMutableBasicBlocks();
    auto &preheader = mutableBlocks[loop.preheader]->instructions;
    auto terminator = std::find_if(
            preheader.begin(), preheader.end(),
            [](const Inst &inst) { return inst.isTerminator(); });
    preheader.insert(
            terminator,
            std::make_move_iterator(expanded.begin()),
            std::make_move_iterator(expanded.end()));
    bool redirected = false;
    for (auto &inst: preheader) {
        const auto *target = inst.op == Op::Br ? asLabel(inst.arg1) : nullptr;
        if (target && target->nameAndId == mutableBlocks[loop.header]->label.nameAndId) {
            inst.arg1 = mutableBlocks[loop.exit]->label.clone();
            redirected = true;
        }
    }
    if (!redirected) {
        return false;
    }

    const std::string headerLabel = mutableBlocks[loop.header]->label.nameAndId;
    const std::string preheaderLabel = mutableBlocks[loop.preheader]->label.nameAndId;
    for (size_t block = 0; block < mutableBlocks.size(); ++block) {
        if (loop.blocks.count(block) != 0) {
            continue;
        }
        for (auto &inst: mutableBlocks[block]->instructions) {
            if (inst.op == Op::Phi) {
                for (auto &incoming: inst.phiIncoming) {
                    if (incoming.predecessor == headerLabel) {
                        incoming.predecessor = preheaderLabel;
                    }
                }
            }
            const auto operands = inst.operands();
            for (const auto &operand: operands) {
                if (operand.role == OperandRole::Definition) {
                    continue;
                }
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                auto replacement = temp ? state.find(temp->id) : state.end();
                if (replacement != state.end()) {
                    inst.setOperand(operand.slot, replacement->second->clone());
                }
            }
        }
    }
    return true;
}

} // namespace

bool simplifyClosedFormLoops(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    auto loopsByHeader = collectNaturalLoops(cfg);
    if (loopsByHeader.empty()) {
        return false;
    }
    std::vector<std::pair<size_t, std::unordered_set<size_t>>> loops;
    loops.reserve(loopsByHeader.size());
    for (auto &[header, blocks]: loopsByHeader) {
        loops.emplace_back(header, std::move(blocks));
    }
    std::sort(loops.begin(), loops.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.second.size() < rhs.second.size();
    });

    const auto labels = blockIndices(function);
    const auto defs = collectTempDefinitions(function);
    const auto knownConstants = collectImmediateConstants(function);
    for (const auto &[header, loopBlocks]: loops) {
        const auto control = analyzeLoopControl(
                function, cfg, header, loopBlocks, labels, defs, knownConstants);
        if (!control) {
            continue;
        }
        const auto iterations = fixedIterationCount(*control, knownConstants);
        if (iterations
            && collapseWholeArrayZeroFill(
                    function, *control, *iterations, knownConstants)) {
            return true;
        }
        if (iterations
            && versionBitReconstructionLoop(
                    function, cfg, *control, *iterations,
                    labels, defs, knownConstants)) {
            return true;
        }
        if (iterations
            && collapseFixedReduction(
                    function, *control, *iterations, defs, knownConstants)) {
            return true;
        }
        if (iterations
            && unrollSmallSingleBlockLoop(
                    function, *control, *iterations)) {
            return true;
        }
        if (collapsePeriodicRemainderSum(
                    function, *control, labels, defs, knownConstants)) {
            return true;
        }
    }
    return false;
}

} // namespace IR
