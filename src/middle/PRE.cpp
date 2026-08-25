#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace IR {
namespace {

bool isPRECandidate(Op op) {
    switch (op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::And:
        case Op::Or:
        case Op::Xor:
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

std::string expressionKey(const Inst &inst) {
    const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
    if (!destination || !isPRECandidate(inst.op)) {
        return {};
    }
    std::string lhs = inst.arg1 ? inst.arg1->valueKey() : "_";
    std::string rhs = inst.arg2 ? inst.arg2->valueKey() : "_";
    if (isCommutative(inst.op) && rhs < lhs) {
        std::swap(lhs, rhs);
    }
    return std::to_string(static_cast<int>(inst.op)) + ":"
           + std::to_string(static_cast<int>(destination->type)) + ":"
           + lhs + ":" + rhs;
}

bool operandsDominatePredecessors(
        const Inst &inst,
        const std::vector<size_t> &predecessors,
        const ControlFlowGraph &cfg,
        const TempDefinitions &definitions) {
    for (int operand: usedTemps(inst)) {
        auto definition = definitions.find(operand);
        if (definition == definitions.end()) {
            return false;
        }
        for (size_t predecessor: predecessors) {
            if (!cfg.dominates(definition->second.block, predecessor)) {
                return false;
            }
        }
    }
    return true;
}

std::unordered_map<std::string, Temp> expressionsAvailableOnEveryEdge(
        const BasicBlock &block) {
    std::unordered_map<std::string, Temp> available;
    for (const auto &inst: block.instructions) {
        if (inst.isTerminator()) {
            break;
        }
        const std::string key = expressionKey(inst);
        const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
        if (!key.empty() && destination) {
            available.insert_or_assign(key, *destination);
        }
    }
    return available;
}

} // namespace

bool eliminatePartialRedundancy(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }

    const auto definitions = collectTempDefinitions(function);
    std::vector<std::unordered_map<std::string, Temp>> available;
    available.reserve(blocks.size());
    for (const auto &block: blocks) {
        available.push_back(expressionsAvailableOnEveryEdge(*block));
    }

    int nextTemp = nextTempId(function);
    for (size_t block = 0; block < blocks.size(); ++block) {
        const auto &predecessors = cfg.predecessors[block];
        if (predecessors.size() < 2) {
            continue;
        }
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size(); ++index) {
            const std::string key = expressionKey(instructions[index]);
            const auto *destination = dynamic_cast<const Temp *>(instructions[index].res.get());
            if (key.empty() || !destination
                || !operandsDominatePredecessors(
                        instructions[index], predecessors, cfg, definitions)) {
                continue;
            }

            size_t existingCount = 0;
            for (size_t predecessor: predecessors) {
                existingCount += available[predecessor].find(key) != available[predecessor].end();
            }
            const size_t missingCount = predecessors.size() - existingCount;
            if (existingCount == 0 || missingCount == 0 || existingCount < missingCount) {
                continue;
            }

            std::vector<PhiIncoming> incoming;
            incoming.reserve(predecessors.size());
            for (size_t predecessor: predecessors) {
                auto existing = available[predecessor].find(key);
                Temp value(0, destination->type);
                if (existing != available[predecessor].end()) {
                    value = existing->second;
                } else {
                    value = Temp(nextTemp++, destination->type);
                    auto clone = Inst(
                            instructions[index].op,
                            value.clone(),
                            instructions[index].arg1 ? instructions[index].arg1->clone() : nullptr,
                            instructions[index].arg2 ? instructions[index].arg2->clone() : nullptr);
                    auto &predecessorInstructions = blocks[predecessor]->instructions;
                    auto insertion = std::find_if(
                            predecessorInstructions.begin(),
                            predecessorInstructions.end(),
                            [](const Inst &inst) { return inst.isTerminator(); });
                    predecessorInstructions.insert(insertion, std::move(clone));
                }
                incoming.emplace_back(
                        blocks[predecessor]->label.nameAndId,
                        value.clone());
            }

            Inst phi(Op::Phi, destination->clone(), nullptr, nullptr);
            phi.phiIncoming = std::move(incoming);
            instructions.erase(instructions.begin() + static_cast<long>(index));
            auto phiInsertion = std::find_if(
                    instructions.begin(), instructions.end(), [](const Inst &inst) {
                        return inst.op != Op::Phi;
                    });
            instructions.insert(phiInsertion, std::move(phi));
            return true;
        }
    }
    return false;
}

} // namespace IR
