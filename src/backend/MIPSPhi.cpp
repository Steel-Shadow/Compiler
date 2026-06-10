#include "backend/MIPSInternal.h"

#include <algorithm>

namespace MIPS::detail {

void FunctionEmitter::collectPhiMoves() {
    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            if (inst.opcode != IR::Opcode::Phi) {
                continue;
            }
            if (neededValues_.find(inst.result) == neededValues_.end()) {
                continue;
            }
            for (const auto &incoming: inst.incoming) {
                phiMoves_[edgeKey(incoming.block, block->name)].push_back({incoming.value, inst.result});
            }
        }
    }
    for (auto &[edge, moves]: phiMoves_) {
        (void) edge;
        moves.erase(std::remove_if(moves.begin(), moves.end(), [](const auto &move) {
            return move.first.text == move.second;
        }), moves.end());
    }
}

bool FunctionEmitter::hasPhiMoves(const std::string &target) const {
    auto it = phiMoves_.find(edgeKey(currentBlock_, target));
    return it != phiMoves_.end() && !it->second.empty();
}

std::string FunctionEmitter::valueLocation(const std::string &name) const {
    auto regIt = frame_.valueRegs.find(name);
    if (regIt != frame_.valueRegs.end()) {
        return "reg:" + regIt->second;
    }
    auto slotIt = frame_.valueSlots.find(name);
    if (slotIt != frame_.valueSlots.end()) {
        return "slot:" + std::to_string(slotIt->second);
    }
    return "";
}

std::string FunctionEmitter::operandLocation(const IR::Operand &operand) const {
    if (operand.text.empty() || operand.text.front() != '%') {
        return "";
    }
    return valueLocation(operand.text);
}

bool FunctionEmitter::isPhysicalSelfMove(const IR::Operand &value, const std::string &result) const {
    std::string source = operandLocation(value);
    std::string dest = valueLocation(result);
    return !source.empty() && source == dest;
}

bool FunctionEmitter::needsParallelCopy(const std::vector<std::pair<IR::Operand, std::string>> &moves) const {
    std::unordered_set<std::string> destinations;
    for (const auto &[value, result]: moves) {
        (void) value;
        std::string dest = valueLocation(result);
        if (!dest.empty()) {
            destinations.insert(dest);
        }
    }
    for (const auto &[value, result]: moves) {
        std::string source = operandLocation(value);
        std::string dest = valueLocation(result);
        if (!source.empty() && source != dest && destinations.find(source) != destinations.end()) {
            return true;
        }
    }
    return false;
}

void FunctionEmitter::emitDirectPhiMove(const IR::Operand &value, const std::string &result) {
    if (isPhysicalSelfMove(value, result)) {
        return;
    }

    std::string destReg = assignedRegister(result);
    if (!destReg.empty()) {
        loadOperand(value, destReg);
        return;
    }

    auto slotIt = frame_.valueSlots.find(result);
    if (slotIt == frame_.valueSlots.end()) {
        loadOperand(value, "$t8");
        storeValue(result, "$t8");
        return;
    }

    if (!value.text.empty() && value.text.front() == '%') {
        std::string sourceReg = assignedRegister(value.text);
        if (!sourceReg.empty()) {
            out_ << "  sw " << sourceReg << ", " << slotIt->second << "($sp)\n";
            return;
        }
    }

    loadOperand(value, "$t8");
    out_ << "  sw $t8, " << slotIt->second << "($sp)\n";
}

void FunctionEmitter::emitPhiMoves(const std::string &target) {
    auto it = phiMoves_.find(edgeKey(currentBlock_, target));
    if (it == phiMoves_.end()) {
        return;
    }
    if (!needsParallelCopy(it->second)) {
        for (const auto &[value, result]: it->second) {
            emitDirectPhiMove(value, result);
        }
        return;
    }
    for (size_t i = 0; i < it->second.size(); ++i) {
        const auto &[value, result] = it->second[i];
        (void) result;
        if (isPhysicalSelfMove(value, result)) {
            out_ << "  move $t8, $zero\n";
            out_ << "  sw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
            continue;
        }
        loadOperand(value, "$t8");
        out_ << "  sw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
    }
    for (size_t i = 0; i < it->second.size(); ++i) {
        const auto &[value, result] = it->second[i];
        (void) value;
        if (isPhysicalSelfMove(value, result)) {
            continue;
        }
        out_ << "  lw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
        storeValue(result, "$t8");
    }
}

void FunctionEmitter::emitCondBranchWithPhi(const IR::Instruction &inst, const std::string &condReg) {
    std::string trueTarget = labelName(inst.operands[1]);
    std::string falseTarget = labelName(inst.operands[2]);
    if (!hasPhiMoves(trueTarget) && !hasPhiMoves(falseTarget)) {
        out_ << "  bne " << condReg << ", $zero, " << labelOf(trueTarget) << "\n";
        out_ << "  j " << labelOf(falseTarget) << "\n";
        return;
    }
    std::string trueEdge = sanitizeLabel(function_.name + "_" + currentBlock_ + "_to_" + trueTarget + "_" + std::to_string(edgeId_++));
    out_ << "  bne " << condReg << ", $zero, " << trueEdge << "\n";
    emitPhiMoves(falseTarget);
    out_ << "  j " << labelOf(falseTarget) << "\n";
    out_ << trueEdge << ":\n";
    emitPhiMoves(trueTarget);
    out_ << "  j " << labelOf(trueTarget) << "\n";
}

} // namespace MIPS::detail
