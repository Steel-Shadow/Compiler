#include "backend/MIPSInternal.h"

#include <algorithm>

namespace MIPS::detail {

void FunctionEmitter::allocateRegisters() {
    ValueSet values = collectRegisterCandidates();
    if (values.empty()) {
        return;
    }

    std::map<std::string, BlockLiveness> liveness = buildLiveness(values);
    callLiveValues_ = collectCallLiveValues(values, liveness);
    std::map<std::string, ValueSet> graph = buildInterferenceGraph(values, liveness);
    colorInterferenceGraph(values, graph);
}

ValueSet FunctionEmitter::collectRegisterCandidates() const {
    ValueSet values;
    for (const auto &param: function_.params) {
        values.insert("%" + param.name);
    }
    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            if (inst.opcode == IR::Opcode::Phi && neededValues_.find(inst.result) == neededValues_.end()) {
                continue;
            }
            if (inst.opcode != IR::Opcode::Alloca && inst.hasResult()) {
                values.insert(inst.result);
            }
        }
    }
    return values;
}

bool FunctionEmitter::isCandidateValue(const IR::Operand &operand, const ValueSet &values) {
    return values.find(operand.text) != values.end();
}

void FunctionEmitter::addOperandUse(const IR::Operand &operand, const ValueSet &values, ValueSet &uses) {
    if (isCandidateValue(operand, values)) {
        uses.insert(operand.text);
    }
}

ValueSet FunctionEmitter::collectNeededValues() const {
    ValueSet needed;
    auto mark = [&needed](const IR::Operand &operand) {
        if (!operand.text.empty() && operand.text.front() == '%' && operand.type != "label") {
            needed.insert(operand.text);
        }
    };

    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            if (inst.opcode == IR::Opcode::Phi) {
                continue;
            }
            for (const auto &operand: inst.operands) {
                mark(operand);
            }
        }
    }

    bool changed;
    do {
        changed = false;
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode != IR::Opcode::Phi || needed.find(inst.result) == needed.end()) {
                    continue;
                }
                for (const auto &incoming: inst.incoming) {
                    if (!incoming.value.text.empty() && incoming.value.text.front() == '%' &&
                        needed.insert(incoming.value.text).second) {
                        changed = true;
                    }
                }
            }
        }
    } while (changed);
    return needed;
}

std::vector<std::string> FunctionEmitter::successorsOf(const IR::BasicBlock &block) const {
    std::vector<std::string> successors;
    if (block.instructions.empty()) {
        return successors;
    }
    const auto &term = block.instructions.back();
    if (term.opcode == IR::Opcode::Br) {
        successors.push_back(labelName(term.operands.front()));
    } else if (term.opcode == IR::Opcode::CondBr) {
        successors.push_back(labelName(term.operands[1]));
        successors.push_back(labelName(term.operands[2]));
    }
    return successors;
}

std::map<std::string, ValueSet> FunctionEmitter::collectPhiDefs(const ValueSet &values) const {
    std::map<std::string, ValueSet> phiDefs;
    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            if (inst.opcode == IR::Opcode::Phi && values.find(inst.result) != values.end()) {
                phiDefs[block->name].insert(inst.result);
            }
        }
    }
    return phiDefs;
}

std::map<std::string, ValueSet> FunctionEmitter::collectPhiEdgeUses(const ValueSet &values) const {
    std::map<std::string, ValueSet> edgeUses;
    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            if (inst.opcode != IR::Opcode::Phi) {
                continue;
            }
            for (const auto &incoming: inst.incoming) {
                if (isCandidateValue(incoming.value, values)) {
                    edgeUses[edgeKey(incoming.block, block->name)].insert(incoming.value.text);
                }
            }
        }
    }
    return edgeUses;
}

std::map<std::string, BlockLiveness> FunctionEmitter::buildLiveness(const ValueSet &values) const {
    std::map<std::string, BlockLiveness> blocks;
    for (const auto &blockPtr: function_.blocks) {
        const auto &block = *blockPtr;
        auto &info = blocks[block.name];
        info.successors = successorsOf(block);
        for (const auto &inst: block.instructions) {
            if (inst.opcode != IR::Opcode::Phi) {
                for (const auto &operand: inst.operands) {
                    if (isCandidateValue(operand, values) && info.def.find(operand.text) == info.def.end()) {
                        info.use.insert(operand.text);
                    }
                }
            }
            if (inst.opcode != IR::Opcode::Alloca && inst.hasResult() && values.find(inst.result) != values.end()) {
                info.def.insert(inst.result);
            }
        }
    }

    auto phiDefs = collectPhiDefs(values);
    auto phiEdgeUses = collectPhiEdgeUses(values);
    bool changed;
    do {
        changed = false;
        for (auto it = function_.blocks.rbegin(); it != function_.blocks.rend(); ++it) {
            const auto &block = **it;
            auto &info = blocks[block.name];
            ValueSet liveOut;
            for (const auto &succ: info.successors) {
                ValueSet succIn = blocks[succ].liveIn;
                auto defIt = phiDefs.find(succ);
                if (defIt != phiDefs.end()) {
                    for (const auto &phiDef: defIt->second) {
                        succIn.erase(phiDef);
                    }
                }
                liveOut.insert(succIn.begin(), succIn.end());
                auto edgeIt = phiEdgeUses.find(edgeKey(block.name, succ));
                if (edgeIt != phiEdgeUses.end()) {
                    liveOut.insert(edgeIt->second.begin(), edgeIt->second.end());
                }
            }

            ValueSet liveIn = info.use;
            for (const auto &value: liveOut) {
                if (info.def.find(value) == info.def.end()) {
                    liveIn.insert(value);
                }
            }

            if (liveIn != info.liveIn || liveOut != info.liveOut) {
                info.liveIn = std::move(liveIn);
                info.liveOut = std::move(liveOut);
                changed = true;
            }
        }
    } while (changed);
    return blocks;
}

ValueSet FunctionEmitter::collectCallLiveValues(const ValueSet &values, const std::map<std::string, BlockLiveness> &liveness) const {
    ValueSet callLive;
    for (const auto &blockPtr: function_.blocks) {
        const auto &block = *blockPtr;
        ValueSet live = liveness.at(block.name).liveOut;
        for (auto instIt = block.instructions.rbegin(); instIt != block.instructions.rend(); ++instIt) {
            const auto &inst = *instIt;
            bool hasDef = inst.opcode != IR::Opcode::Alloca && inst.hasResult() && values.find(inst.result) != values.end();
            if (inst.opcode == IR::Opcode::Call) {
                ValueSet liveAcross = live;
                if (hasDef) {
                    liveAcross.erase(inst.result);
                }
                callLive.insert(liveAcross.begin(), liveAcross.end());
            }
            if (hasDef) {
                live.erase(inst.result);
            }
            if (inst.opcode != IR::Opcode::Phi) {
                for (const auto &operand: inst.operands) {
                    addOperandUse(operand, values, live);
                }
            }
        }
    }
    return callLive;
}

void FunctionEmitter::addInterference(std::map<std::string, ValueSet> &graph,
                            const std::string &lhs,
                            const std::string &rhs) {
    if (lhs == rhs) {
        return;
    }
    graph[lhs].insert(rhs);
    graph[rhs].insert(lhs);
}

std::map<std::string, ValueSet> FunctionEmitter::buildInterferenceGraph(const ValueSet &values,
                                                       const std::map<std::string, BlockLiveness> &liveness) const {
    std::map<std::string, ValueSet> graph;
    for (const auto &value: values) {
        graph[value];
    }
    for (size_t i = 0; i < function_.params.size(); ++i) {
        std::string lhs = "%" + function_.params[i].name;
        if (values.find(lhs) == values.end()) {
            continue;
        }
        for (size_t j = i + 1; j < function_.params.size(); ++j) {
            std::string rhs = "%" + function_.params[j].name;
            if (values.find(rhs) != values.end()) {
                addInterference(graph, lhs, rhs);
            }
        }
    }

    for (const auto &blockPtr: function_.blocks) {
        const auto &block = *blockPtr;
        ValueSet live = liveness.at(block.name).liveOut;
        for (auto instIt = block.instructions.rbegin(); instIt != block.instructions.rend(); ++instIt) {
            const auto &inst = *instIt;
            bool hasDef = inst.opcode != IR::Opcode::Alloca && inst.hasResult() && values.find(inst.result) != values.end();
            if (hasDef) {
                for (const auto &liveValue: live) {
                    addInterference(graph, inst.result, liveValue);
                }
                live.erase(inst.result);
            }
            if (inst.opcode != IR::Opcode::Phi) {
                for (const auto &operand: inst.operands) {
                    addOperandUse(operand, values, live);
                }
            }
        }
    }
    return graph;
}

const std::vector<std::string> &FunctionEmitter::allowedRegistersFor(const std::string &value) const {
    return callLiveValues_.find(value) == callLiveValues_.end() ? allocatableRegs_ : calleeSavedRegs_;
}

void FunctionEmitter::colorInterferenceGraph(const ValueSet &values, const std::map<std::string, ValueSet> &graph) {
    const int k = static_cast<int>(allocatableRegs_.size());
    std::map<std::string, ValueSet> workGraph = graph;
    std::vector<std::string> stack;
    stack.reserve(values.size());

    while (!workGraph.empty()) {
        auto chosen = workGraph.end();
        for (auto it = workGraph.begin(); it != workGraph.end(); ++it) {
            if (static_cast<int>(it->second.size()) < k) {
                chosen = it;
                break;
            }
        }
        if (chosen == workGraph.end()) {
            chosen = std::max_element(workGraph.begin(), workGraph.end(), [](const auto &lhs, const auto &rhs) {
                if (lhs.second.size() != rhs.second.size()) {
                    return lhs.second.size() < rhs.second.size();
                }
                return lhs.first < rhs.first;
            });
        }

        std::string node = chosen->first;
        stack.push_back(node);
        for (const auto &neighbor: chosen->second) {
            auto neighborIt = workGraph.find(neighbor);
            if (neighborIt != workGraph.end()) {
                neighborIt->second.erase(node);
            }
        }
        workGraph.erase(chosen);
    }

    std::map<std::string, std::string> colors;
    while (!stack.empty()) {
        std::string node = stack.back();
        stack.pop_back();
        std::set<std::string> unavailable;
        auto graphIt = graph.find(node);
        if (graphIt != graph.end()) {
            for (const auto &neighbor: graphIt->second) {
                auto colorIt = colors.find(neighbor);
                if (colorIt != colors.end()) {
                    unavailable.insert(colorIt->second);
                }
            }
        }
        for (const auto &reg: allowedRegistersFor(node)) {
            if (unavailable.find(reg) == unavailable.end()) {
                colors[node] = reg;
                frame_.valueRegs[node] = reg;
                break;
            }
        }
    }
}

} // namespace MIPS::detail
