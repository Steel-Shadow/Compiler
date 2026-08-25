#include "middle/Optimize.h"

#include "errorHandler/Error.h"
#include "middle/Analysis.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

const Temp *asTemp(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const Temp *>(element.get());
}

const ConstVal *asConstant(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const ConstVal *>(element.get());
}

bool isCommutative(Op op) {
    return op == Op::Add || op == Op::Mul || op == Op::And || op == Op::Or
           || op == Op::Eql || op == Op::Neq;
}

bool isValueNumberCandidate(Op op) {
    switch (op) {
        case Op::LoadImd:
        case Op::NewMove:
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd:
        case Op::Neg:
        case Op::Mult4:
        case Op::Not:
            return true;
        default:
            return false;
    }
}

bool isSpeculatable(Op op) {
    switch (op) {
        case Op::LoadImd:
        case Op::NewMove:
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::And:
        case Op::Or:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd:
        case Op::Neg:
        case Op::Mult4:
        case Op::Not:
            return true;
        default:
            return false;
    }
}

bool hasPhysicalRegisterOperand(const Inst &inst) {
    for (const auto &operand: inst.operands()) {
        if (operand.role == OperandRole::Definition) {
            continue;
        }
        const auto *temp = dynamic_cast<const Temp *>(operand.value);
        if (temp && temp->id < 0) {
            return true;
        }
    }
    return false;
}

bool isMovable(const Inst &inst) {
    return isSpeculatable(inst.op) && !hasPhysicalRegisterOperand(inst);
}

std::unordered_map<int, int> definitionCounts(const Function &function) {
    std::unordered_map<int, int> counts;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = definedTemp(inst)) {
                ++counts[*definition];
            }
        }
    }
    return counts;
}

std::string expressionKey(
        const Inst &inst,
        const std::unordered_map<int, std::string> &constantValues) {
    const auto *destination = asTemp(inst.res);
    if (!destination || !isValueNumberCandidate(inst.op)) {
        return {};
    }

    auto operandKey = [&](const std::unique_ptr<Element> &operand) {
        const auto *temp = asTemp(operand);
        if (temp) {
            auto constant = constantValues.find(temp->id);
            if (constant != constantValues.end()) {
                return constant->second;
            }
        }
        return operand ? operand->valueKey() : std::string("_");
    };

    std::string lhs = operandKey(inst.arg1);
    std::string rhs = operandKey(inst.arg2);
    if (isCommutative(inst.op) && rhs < lhs) {
        std::swap(lhs, rhs);
    }
    return std::to_string(static_cast<int>(inst.op)) + ":"
           + std::to_string(static_cast<int>(destination->type)) + ":"
           + lhs + ":" + rhs;
}

bool eliminateGlobalRedundancy(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto counts = definitionCounts(function);
    std::unordered_map<int, std::string> constantValues;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            const auto definition = definedTemp(inst);
            const auto *constant = asConstant(inst.arg1);
            if (definition && counts.at(*definition) == 1
                && inst.op == Op::LoadImd && constant) {
                constantValues[*definition] = "constant:"
                                              + std::to_string(static_cast<int>(constant->type)) + ":"
                                              + std::to_string(constant->value);
            }
        }
    }

    struct Representative {
        int temp;
        Type type;
        size_t block;
        bool movable;
    };
    std::unordered_map<std::string, Representative> values;
    std::unordered_map<int, Temp> replacements;
    auto resolveReplacement = [&](const Temp &value) {
        Temp result(value);
        std::unordered_set<int> visited;
        while (result.id >= 0 && visited.insert(result.id).second) {
            auto replacement = replacements.find(result.id);
            if (replacement == replacements.end()) {
                break;
            }
            result = replacement->second;
        }
        return result;
    };
    auto rewriteUses = [&](Inst &inst) {
        const auto operands = inst.operands();
        for (const auto &operand: operands) {
            if (operand.role == OperandRole::Definition) {
                continue;
            }
            const auto *temp = dynamic_cast<const Temp *>(operand.value);
            if (!temp || temp->id < 0) {
                continue;
            }
            const Temp replacement = resolveReplacement(*temp);
            if (replacement.id != temp->id) {
                inst.setOperand(operand.slot, std::make_unique<Temp>(replacement));
            }
        }
    };

    bool changed = false;
    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            rewriteUses(inst);
            const auto *destination = asTemp(inst.res);
            const std::string key = expressionKey(inst, constantValues);
            bool validOperands = !hasPhysicalRegisterOperand(inst);
            for (int operand: usedTemps(inst)) {
                auto count = counts.find(operand);
                validOperands = validOperands && count != counts.end() && count->second == 1;
            }
            auto count = destination ? counts.find(destination->id) : counts.end();
            if (key.empty() || !destination || count == counts.end() || count->second != 1
                || !validOperands) {
                ++index;
                continue;
            }

            auto existing = values.find(key);
            if (existing == values.end()) {
                values.emplace(key,
                               Representative{destination->id,
                                              destination->type,
                                              block,
                                              isMovable(inst)});
                ++index;
                continue;
            }

            const bool globallyMovable = existing->second.movable && isMovable(inst);
            if (!globallyMovable && !cfg.dominates(existing->second.block, block)) {
                ++index;
                continue;
            }
            Temp replacement(existing->second.temp, existing->second.type);
            replacements.insert_or_assign(destination->id, resolveReplacement(replacement));
            instructions.erase(instructions.begin() + static_cast<long>(index));
            changed = true;
        }
    }
    if (changed) {
        for (auto &block: blocks) {
            for (auto &inst: block->instructions) {
                rewriteUses(inst);
            }
        }
    }
    return changed;
}

size_t dominatorLCA(const ControlFlowGraph &cfg, size_t lhs, size_t rhs) {
    if (lhs == ControlFlowGraph::NoBlock) {
        return rhs;
    }
    if (rhs == ControlFlowGraph::NoBlock) {
        return lhs;
    }
    while (lhs != rhs) {
        if (cfg.dominatorDepth[lhs] > cfg.dominatorDepth[rhs]) {
            lhs = cfg.immediateDominator[lhs];
        } else if (cfg.dominatorDepth[rhs] > cfg.dominatorDepth[lhs]) {
            rhs = cfg.immediateDominator[rhs];
        } else {
            lhs = cfg.immediateDominator[lhs];
            rhs = cfg.immediateDominator[rhs];
        }
        if (lhs == ControlFlowGraph::NoBlock || rhs == ControlFlowGraph::NoBlock) {
            return 0;
        }
    }
    return lhs;
}

bool scheduleGlobalCodeMotion(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto counts = definitionCounts(function);
    const auto loopDepth = IR::computeLoopDepths(cfg);
    const auto &blocks = function.getBasicBlocks();

    struct Definition {
        size_t block;
        const Inst *instruction;
    };
    std::unordered_map<int, Definition> definitions;
    for (size_t block = 0; block < blocks.size(); ++block) {
        for (const auto &inst: blocks[block]->instructions) {
            if (auto definition = definedTemp(inst)) {
                definitions[*definition] = {block, &inst};
            }
        }
    }

    std::unordered_map<int, const Inst *> nodes;
    std::unordered_map<int, size_t> originalBlock;
    for (const auto &[temp, definition]: definitions) {
        auto count = counts.find(temp);
        if (count != counts.end() && count->second == 1
            && isMovable(*definition.instruction)) {
            nodes[temp] = definition.instruction;
            originalBlock[temp] = definition.block;
        }
    }
    if (nodes.empty()) {
        return false;
    }

    std::unordered_map<int, size_t> earliest;
    std::unordered_map<int, int> earlyState;
    std::function<size_t(int)> scheduleEarly = [&](int temp) -> size_t {
        if (auto position = earliest.find(temp); position != earliest.end()) {
            return position->second;
        }
        if (earlyState[temp] == 1) {
            return originalBlock.at(temp);
        }
        earlyState[temp] = 1;
        size_t position = 0;
        for (int operand: usedTemps(*nodes.at(temp))) {
            auto definition = definitions.find(operand);
            if (definition == definitions.end()) {
                continue;
            }
            const size_t operandBlock = nodes.find(operand) != nodes.end()
                                                ? scheduleEarly(operand)
                                                : definition->second.block;
            if (cfg.dominatorDepth[operandBlock] > cfg.dominatorDepth[position]) {
                position = operandBlock;
            }
        }
        earlyState[temp] = 2;
        earliest[temp] = position;
        return position;
    };
    for (const auto &[temp, inst]: nodes) {
        (void) inst;
        scheduleEarly(temp);
    }

    struct UseSite {
        int movableUser{-1};
        size_t pinnedBlock{ControlFlowGraph::NoBlock};
    };
    std::unordered_map<int, std::vector<UseSite>> uses;
    std::unordered_map<std::string, size_t> labelToBlock;
    for (size_t block = 0; block < blocks.size(); ++block) {
        labelToBlock.emplace(blocks[block]->label.nameAndId, block);
    }
    for (size_t block = 0; block < blocks.size(); ++block) {
        for (const auto &inst: blocks[block]->instructions) {
            const auto userDefinition = definedTemp(inst);
            for (const auto &operand: inst.operands()) {
                if (operand.role == OperandRole::Definition) {
                    continue;
                }
                const auto *value = dynamic_cast<const Temp *>(operand.value);
                if (!value || nodes.find(value->id) == nodes.end()) {
                    continue;
                }
                if (inst.op == Op::Phi && operand.slot >= 3) {
                    const size_t incoming = operand.slot - 3;
                    auto predecessor = incoming < inst.phiIncoming.size()
                                               ? labelToBlock.find(inst.phiIncoming[incoming].predecessor)
                                               : labelToBlock.end();
                    if (predecessor != labelToBlock.end()) {
                        uses[value->id].push_back({-1, predecessor->second});
                    }
                } else if (userDefinition && nodes.find(*userDefinition) != nodes.end()) {
                    uses[value->id].push_back({*userDefinition, ControlFlowGraph::NoBlock});
                } else {
                    uses[value->id].push_back({-1, block});
                }
            }
        }
    }

    std::unordered_map<int, size_t> targetBlock;
    std::unordered_map<int, int> lateState;
    std::function<size_t(int)> scheduleLate = [&](int temp) -> size_t {
        if (auto position = targetBlock.find(temp); position != targetBlock.end()) {
            return position->second;
        }
        if (lateState[temp] == 1) {
            return originalBlock.at(temp);
        }
        lateState[temp] = 1;
        size_t latest = ControlFlowGraph::NoBlock;
        auto tempUses = uses.find(temp);
        if (tempUses != uses.end()) {
            for (const auto &use: tempUses->second) {
                const size_t useBlock = use.movableUser >= 0
                                                ? scheduleLate(use.movableUser)
                                                : use.pinnedBlock;
                latest = dominatorLCA(cfg, latest, useBlock);
            }
        }
        if (latest == ControlFlowGraph::NoBlock
            || !cfg.dominates(earliest.at(temp), latest)) {
            latest = originalBlock.at(temp);
        }

        size_t best = latest;
        size_t candidate = latest;
        while (true) {
            if (loopDepth[candidate] < loopDepth[best]) {
                best = candidate;
            }
            if (candidate == earliest.at(temp)) {
                break;
            }
            candidate = cfg.immediateDominator[candidate];
            if (candidate == ControlFlowGraph::NoBlock) {
                best = originalBlock.at(temp);
                break;
            }
        }
        lateState[temp] = 2;
        targetBlock[temp] = best;
        return best;
    };
    for (const auto &[temp, inst]: nodes) {
        (void) inst;
        scheduleLate(temp);
    }

    const bool moved = std::any_of(targetBlock.begin(), targetBlock.end(), [&](const auto &entry) {
        return entry.second != originalBlock.at(entry.first);
    });
    if (!moved) {
        return false;
    }

    std::unordered_map<int, std::vector<int>> dependencies;
    for (const auto &[temp, inst]: nodes) {
        for (int operand: usedTemps(*inst)) {
            if (nodes.find(operand) != nodes.end()) {
                dependencies[temp].push_back(operand);
            }
        }
        std::sort(dependencies[temp].begin(), dependencies[temp].end());
    }

    std::map<int, Inst> scheduled;
    auto &mutableBlocks = function.getMutableBasicBlocks();
    for (auto &block: mutableBlocks) {
        auto &instructions = block->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto definition = definedTemp(instructions[index]);
            if (definition && nodes.find(*definition) != nodes.end()) {
                scheduled.emplace(*definition, std::move(instructions[index]));
                instructions.erase(instructions.begin() + static_cast<long>(index));
            } else {
                ++index;
            }
        }
    }

    std::vector<std::vector<int>> targeted(mutableBlocks.size());
    for (const auto &[temp, target]: targetBlock) {
        targeted[target].push_back(temp);
    }
    for (auto &temps: targeted) {
        std::sort(temps.begin(), temps.end());
    }

    std::unordered_set<int> emitted;
    std::unordered_set<int> emitting;
    for (size_t block = 0; block < mutableBlocks.size(); ++block) {
        std::vector<Inst> pinned = std::move(mutableBlocks[block]->instructions);
        std::vector<Inst> output;
        output.reserve(pinned.size() + targeted[block].size());

        std::function<void(int)> emit = [&](int temp) {
            if (emitted.find(temp) != emitted.end() || targetBlock.at(temp) != block) {
                return;
            }
            if (!emitting.insert(temp).second) {
                Error::raise("Cycle among movable GCM nodes");
                return;
            }
            for (int dependency: dependencies[temp]) {
                emit(dependency);
            }
            emitting.erase(temp);
            emitted.insert(temp);
            output.emplace_back(std::move(scheduled.at(temp)));
        };

        for (auto &inst: pinned) {
            if (inst.op != Op::Phi) {
                for (int operand: usedTemps(inst)) {
                    if (targetBlock.find(operand) != targetBlock.end()) {
                        emit(operand);
                    }
                }
            }
            if (inst.isTerminator()) {
                for (int temp: targeted[block]) {
                    emit(temp);
                }
            }
            output.emplace_back(std::move(inst));
        }
        for (int temp: targeted[block]) {
            emit(temp);
        }
        mutableBlocks[block]->instructions = std::move(output);
    }
    return true;
}

} // namespace

bool globalValueNumberingCodeMotion(Function &function) {
    bool numbered = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const bool changed = eliminateGlobalRedundancy(function);
        numbered = numbered || changed;
        if (!changed) {
            break;
        }
    }
    const bool moved = scheduleGlobalCodeMotion(function);
    return numbered || moved;
}

} // namespace IR
