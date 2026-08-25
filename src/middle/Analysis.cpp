#include "middle/Analysis.h"

#include "middle/IRUtils.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <string>

namespace IR {
namespace {

void appendUnique(std::vector<size_t> &values, size_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void appendLabelTarget(const std::unique_ptr<Element> &element,
                       const std::unordered_map<std::string, size_t> &labelToBlock,
                       std::vector<size_t> &successors) {
    const auto *label = asLabel(element);
    if (!label) {
        return;
    }
    auto target = labelToBlock.find(label->nameAndId);
    if (target != labelToBlock.end()) {
        appendUnique(successors, target->second);
    }
}

template<class Set>
void unionInto(Set &target, const Set &source) {
    target.insert(source.begin(), source.end());
}

NaturalLoops findNaturalLoopsImpl(const ControlFlowGraph &cfg, bool reachableOnly) {
    NaturalLoops loops;
    for (size_t latch = 0; latch < cfg.successors.size(); ++latch) {
        if (reachableOnly
            && (latch >= cfg.reachable.size() || !cfg.reachable[latch])) {
            continue;
        }
        for (size_t header: cfg.successors[latch]) {
            if (!cfg.dominates(header, latch)) {
                continue;
            }
            auto &loop = loops[header];
            loop.insert(header);
            loop.insert(latch);
            std::vector<size_t> work{latch};
            while (!work.empty()) {
                const size_t block = work.back();
                work.pop_back();
                for (size_t predecessor: cfg.predecessors[block]) {
                    if (loop.insert(predecessor).second && predecessor != header) {
                        work.push_back(predecessor);
                    }
                }
            }
        }
    }
    return loops;
}

VarSet escapedScalarVariables(const Function &function) {
    VarSet escaped;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            switch (inst.op) {
                case Op::StoreDynamic:
                case Op::MemZero:
                case Op::LoadDynamic:
                case Op::PushAddressParam:
                case Op::PrintStr:
                case Op::GetString: {
                    const auto *var = asVar(inst.arg1);
                    if (var && isRegisterCandidate(*var)) {
                        escaped.insert(*var);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    return escaped;
}

std::optional<Var> definedVariable(const Inst &inst, const VarSet &candidates) {
    if (inst.op != Op::Store) {
        return std::nullopt;
    }
    const auto *var = asVar(inst.arg1);
    if (!var || candidates.find(*var) == candidates.end()) {
        return std::nullopt;
    }
    return *var;
}

VarSet usedVariables(const Inst &inst, const VarSet &candidates) {
    VarSet result;
    if (inst.op != Op::Load) {
        return result;
    }
    const auto *var = asVar(inst.arg1);
    if (var && candidates.find(*var) != candidates.end()) {
        result.insert(*var);
    }
    return result;
}

} // namespace

bool ControlFlowGraph::dominates(size_t dominator, size_t block) const {
    return block < dominators.size() && dominators[block].find(dominator) != dominators[block].end();
}

ControlFlowGraph buildControlFlowGraph(const Function &function) {
    ControlFlowGraph cfg;
    const auto &blocks = function.getBasicBlocks();
    const size_t blockCount = blocks.size();
    cfg.successors.resize(blockCount);
    cfg.predecessors.resize(blockCount);
    cfg.dominators.resize(blockCount);
    cfg.immediateDominator.assign(blockCount, ControlFlowGraph::NoBlock);
    cfg.dominatorTree.resize(blockCount);
    cfg.dominanceFrontier.resize(blockCount);
    cfg.dominatorDepth.assign(blockCount, 0);
    cfg.reachable.assign(blockCount, false);

    if (blocks.empty()) {
        return cfg;
    }

    std::unordered_map<std::string, size_t> labelToBlock;
    for (size_t i = 0; i < blockCount; ++i) {
        labelToBlock.emplace(blocks[i]->label.nameAndId, i);
    }

    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        bool hasUnconditionalExit = false;
        for (const auto &inst: blocks[blockIndex]->instructions) {
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                appendLabelTarget(inst.arg2, labelToBlock, cfg.successors[blockIndex]);
            } else if (inst.op == Op::Br) {
                appendLabelTarget(inst.arg1, labelToBlock, cfg.successors[blockIndex]);
                hasUnconditionalExit = true;
                break;
            } else if (inst.op == Op::Ret || inst.op == Op::RetMain) {
                hasUnconditionalExit = true;
                break;
            }
        }
        if (!hasUnconditionalExit && blockIndex + 1 < blockCount) {
            appendUnique(cfg.successors[blockIndex], blockIndex + 1);
        }
    }

    for (size_t block = 0; block < blockCount; ++block) {
        for (size_t successor: cfg.successors[block]) {
            appendUnique(cfg.predecessors[successor], block);
        }
    }

    std::vector<bool> visited(blockCount, false);
    std::vector<size_t> postOrder;
    std::function<void(size_t)> dfs = [&](size_t block) {
        if (visited[block]) {
            return;
        }
        visited[block] = true;
        cfg.reachable[block] = true;
        for (size_t successor: cfg.successors[block]) {
            dfs(successor);
        }
        postOrder.push_back(block);
    };
    dfs(0);
    cfg.reversePostOrder.assign(postOrder.rbegin(), postOrder.rend());

    std::vector<size_t> rpoNumber(blockCount, ControlFlowGraph::NoBlock);
    for (size_t index = 0; index < cfg.reversePostOrder.size(); ++index) {
        rpoNumber[cfg.reversePostOrder[index]] = index;
    }

    std::vector<size_t> idom(blockCount, ControlFlowGraph::NoBlock);
    idom[0] = 0;
    auto intersect = [&](size_t lhs, size_t rhs) {
        while (lhs != rhs) {
            while (rpoNumber[lhs] > rpoNumber[rhs]) {
                lhs = idom[lhs];
            }
            while (rpoNumber[rhs] > rpoNumber[lhs]) {
                rhs = idom[rhs];
            }
        }
        return lhs;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t order = 1; order < cfg.reversePostOrder.size(); ++order) {
            const size_t block = cfg.reversePostOrder[order];
            size_t next = ControlFlowGraph::NoBlock;
            for (size_t predecessor: cfg.predecessors[block]) {
                if (idom[predecessor] == ControlFlowGraph::NoBlock) {
                    continue;
                }
                next = next == ControlFlowGraph::NoBlock
                               ? predecessor
                               : intersect(predecessor, next);
            }
            if (next != ControlFlowGraph::NoBlock && idom[block] != next) {
                idom[block] = next;
                changed = true;
            }
        }
    }

    cfg.dominators[0].insert(0);
    for (size_t block = 1; block < blockCount; ++block) {
        if (!cfg.reachable[block]) {
            cfg.dominators[block].insert(block);
            continue;
        }
        cfg.immediateDominator[block] = idom[block];
        size_t cursor = block;
        while (cursor != ControlFlowGraph::NoBlock) {
            cfg.dominators[block].insert(cursor);
            if (cursor == 0) {
                break;
            }
            cursor = idom[cursor];
        }
        if (idom[block] != ControlFlowGraph::NoBlock) {
            cfg.dominatorTree[idom[block]].push_back(block);
        }
    }

    std::queue<size_t> depthWork;
    depthWork.push(0);
    while (!depthWork.empty()) {
        const size_t block = depthWork.front();
        depthWork.pop();
        for (size_t child: cfg.dominatorTree[block]) {
            cfg.dominatorDepth[child] = cfg.dominatorDepth[block] + 1;
            depthWork.push(child);
        }
    }

    for (size_t block = 0; block < blockCount; ++block) {
        if (!cfg.reachable[block]) {
            continue;
        }
        if (cfg.predecessors[block].size() < 2) {
            continue;
        }
        for (size_t predecessor: cfg.predecessors[block]) {
            if (!cfg.reachable[predecessor]) {
                continue;
            }
            size_t runner = predecessor;
            while (runner != cfg.immediateDominator[block]
                   && runner != ControlFlowGraph::NoBlock) {
                appendUnique(cfg.dominanceFrontier[runner], block);
                runner = cfg.immediateDominator[runner];
            }
        }
    }
    for (auto &frontier: cfg.dominanceFrontier) {
        std::sort(frontier.begin(), frontier.end());
    }
    return cfg;
}

NaturalLoops collectNaturalLoops(const ControlFlowGraph &cfg) {
    return findNaturalLoopsImpl(cfg, false);
}

std::vector<size_t> computeLoopDepths(const ControlFlowGraph &cfg) {
    std::vector<size_t> depths(cfg.successors.size(), 0);
    const auto loops = findNaturalLoopsImpl(cfg, true);
    for (const auto &[header, loop]: loops) {
        (void) header;
        for (size_t block: loop) {
            ++depths[block];
        }
    }
    return depths;
}

bool verifySSA(const Function &function, std::string *reason) {
    auto fail = [&](std::string message) {
        if (reason) {
            *reason = std::move(message);
        }
        return false;
    };
    const auto &blocks = function.getBasicBlocks();
    if (blocks.empty()) {
        return fail("function has no entry block");
    }

    const auto cfg = buildControlFlowGraph(function);
    std::unordered_map<std::string, size_t> labels;
    for (size_t block = 0; block < blocks.size(); ++block) {
        if (!labels.emplace(blocks[block]->label.nameAndId, block).second) {
            return fail("duplicate block label " + blocks[block]->label.nameAndId);
        }
    }

    struct Definition {
        size_t block;
        size_t instruction;
    };
    std::unordered_map<int, Definition> definitions;
    for (size_t block = 0; block < blocks.size(); ++block) {
        bool sawNonPhi = false;
        bool sawConditionalTerminator = false;
        bool sawUnconditionalTerminator = false;
        for (size_t index = 0; index < blocks[block]->instructions.size(); ++index) {
            const auto &inst = blocks[block]->instructions[index];
            if (!inst.isScopeMarker() && sawUnconditionalTerminator) {
                return fail("instruction follows a terminator in block "
                            + blocks[block]->label.nameAndId);
            }
            if (!inst.isScopeMarker() && sawConditionalTerminator && inst.op != Op::Br) {
                return fail("instruction follows a conditional terminator in block "
                            + blocks[block]->label.nameAndId);
            }
            if (inst.op == Op::Phi && sawNonPhi) {
                return fail("phi is not at the beginning of block "
                            + blocks[block]->label.nameAndId);
            }
            sawNonPhi = sawNonPhi || inst.op != Op::Phi;
            if (auto definition = definedTemp(inst)) {
                if (!definitions.emplace(*definition, Definition{block, index}).second) {
                    return fail("temporary %" + std::to_string(*definition)
                                + " has multiple definitions");
                }
            }
            for (const auto &operand: inst.operands()) {
                if (operand.role != OperandRole::Target) {
                    continue;
                }
                const auto *target = dynamic_cast<const Label *>(operand.value);
                if (!target || labels.find(target->nameAndId) == labels.end()) {
                    return fail("branch targets a missing block");
                }
            }
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                sawConditionalTerminator = true;
            } else if (inst.isUnconditionalTerminator()) {
                sawUnconditionalTerminator = true;
            }
        }
    }

    for (size_t block = 0; block < blocks.size(); ++block) {
        std::unordered_set<std::string> predecessorLabels;
        for (size_t predecessor: cfg.predecessors[block]) {
            predecessorLabels.insert(blocks[predecessor]->label.nameAndId);
        }
        for (size_t index = 0; index < blocks[block]->instructions.size(); ++index) {
            const auto &inst = blocks[block]->instructions[index];
            if (inst.op == Op::Phi) {
                std::unordered_set<std::string> incomingLabels;
                for (const auto &incoming: inst.phiIncoming) {
                    if (predecessorLabels.find(incoming.predecessor) == predecessorLabels.end()
                        || !incomingLabels.insert(incoming.predecessor).second) {
                        return fail("phi has a missing or duplicate predecessor in block "
                                    + blocks[block]->label.nameAndId);
                    }
                }
                if (incomingLabels.size() != predecessorLabels.size()) {
                    return fail("phi does not cover every predecessor of block "
                                + blocks[block]->label.nameAndId);
                }
            }

            for (const auto &operand: inst.operands()) {
                if (operand.role == OperandRole::Definition) {
                    continue;
                }
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                if (!temp || temp->id < 0) {
                    continue;
                }
                auto definition = definitions.find(temp->id);
                if (definition == definitions.end()) {
                    return fail("temporary %" + std::to_string(temp->id)
                                + " is used without a definition");
                }
                if (inst.op == Op::Phi && operand.slot >= 3) {
                    const size_t incomingIndex = operand.slot - 3;
                    if (incomingIndex >= inst.phiIncoming.size()) {
                        return fail("phi operand index is invalid");
                    }
                    auto predecessor = labels.find(inst.phiIncoming[incomingIndex].predecessor);
                    if (predecessor == labels.end()
                        || !cfg.dominates(definition->second.block, predecessor->second)) {
                        return fail("phi incoming value does not dominate its edge");
                    }
                } else if (definition->second.block == block) {
                    if (definition->second.instruction >= index) {
                        return fail("temporary %" + std::to_string(temp->id)
                                    + " is used before its definition");
                    }
                } else if (!cfg.dominates(definition->second.block, block)) {
                    return fail("temporary %" + std::to_string(temp->id)
                                + " does not dominate its use");
                }
            }
        }
    }
    return true;
}

TempLiveness analyzeTempLiveness(const Function &function, const ControlFlowGraph &cfg) {
    TempLiveness result;
    const auto &blocks = function.getBasicBlocks();
    const size_t blockCount = blocks.size();
    result.liveIn.resize(blockCount);
    result.liveOut.resize(blockCount);
    std::vector<TempSet> uses(blockCount);
    std::vector<TempSet> definitions(blockCount);

    for (size_t block = 0; block < blockCount; ++block) {
        for (const auto &inst: blocks[block]->instructions) {
            for (int temp: usedTemps(inst)) {
                if (definitions[block].find(temp) == definitions[block].end()) {
                    uses[block].insert(temp);
                }
            }
            if (auto definition = definedTemp(inst)) {
                definitions[block].insert(*definition);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t reverse = blockCount; reverse-- > 0;) {
            TempSet nextOut;
            for (size_t successor: cfg.successors[reverse]) {
                unionInto(nextOut, result.liveIn[successor]);
            }
            TempSet nextIn = uses[reverse];
            for (int temp: nextOut) {
                if (definitions[reverse].find(temp) == definitions[reverse].end()) {
                    nextIn.insert(temp);
                }
            }
            if (nextIn != result.liveIn[reverse] || nextOut != result.liveOut[reverse]) {
                result.liveIn[reverse] = std::move(nextIn);
                result.liveOut[reverse] = std::move(nextOut);
                changed = true;
            }
        }
    }

    for (size_t block = 0; block < blockCount; ++block) {
        TempSet live = result.liveOut[block];
        const auto &instructions = blocks[block]->instructions;
        for (size_t reverse = instructions.size(); reverse-- > 0;) {
            const auto &inst = instructions[reverse];
            result.liveAfter.emplace(&inst, live);
            if (auto definition = definedTemp(inst)) {
                live.erase(*definition);
            }
            unionInto(live, usedTemps(inst));
        }
    }
    return result;
}

bool isRegisterCandidate(const Var &var) {
    return var.depth > 0 && var.dims.empty() && !var.storesAddress;
}

VariableLiveness analyzeVariableLiveness(const Function &function, const ControlFlowGraph &cfg) {
    VariableLiveness result;
    const auto &blocks = function.getBasicBlocks();
    const size_t blockCount = blocks.size();
    const auto escaped = escapedScalarVariables(function);

    for (const auto &block: blocks) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                const auto *var = dynamic_cast<const Var *>(operand.value);
                if (var && isRegisterCandidate(*var) && escaped.find(*var) == escaped.end()) {
                    result.candidates.insert(*var);
                }
            }
        }
    }
    for (const auto &param: function.getParams()) {
        Var var(param.name, 1, false, param.dims, param.type, !param.dims.empty());
        if (isRegisterCandidate(var) && escaped.find(var) == escaped.end()) {
            result.candidates.insert(std::move(var));
        }
    }

    result.liveIn.resize(blockCount);
    result.liveOut.resize(blockCount);
    std::vector<VarSet> uses(blockCount);
    std::vector<VarSet> definitions(blockCount);

    for (size_t block = 0; block < blockCount; ++block) {
        for (const auto &inst: blocks[block]->instructions) {
            for (const auto &var: usedVariables(inst, result.candidates)) {
                if (definitions[block].find(var) == definitions[block].end()) {
                    uses[block].insert(var);
                }
            }
            if (auto definition = definedVariable(inst, result.candidates)) {
                definitions[block].insert(*definition);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t reverse = blockCount; reverse-- > 0;) {
            VarSet nextOut;
            for (size_t successor: cfg.successors[reverse]) {
                unionInto(nextOut, result.liveIn[successor]);
            }
            VarSet nextIn = uses[reverse];
            for (const auto &var: nextOut) {
                if (definitions[reverse].find(var) == definitions[reverse].end()) {
                    nextIn.insert(var);
                }
            }
            if (nextIn != result.liveIn[reverse] || nextOut != result.liveOut[reverse]) {
                result.liveIn[reverse] = std::move(nextIn);
                result.liveOut[reverse] = std::move(nextOut);
                changed = true;
            }
        }
    }

    for (size_t block = 0; block < blockCount; ++block) {
        VarSet live = result.liveOut[block];
        const auto &instructions = blocks[block]->instructions;
        for (size_t reverse = instructions.size(); reverse-- > 0;) {
            const auto &inst = instructions[reverse];
            result.liveAfter.emplace(&inst, live);
            if (auto definition = definedVariable(inst, result.candidates)) {
                live.erase(*definition);
            }
            unionInto(live, usedVariables(inst, result.candidates));
        }
    }
    return result;
}

} // namespace IR
