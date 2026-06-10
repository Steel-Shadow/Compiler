#include "backend/MIPSInternal.h"

namespace MIPS::detail {

FunctionEmitter::FunctionEmitter(const IR::Function &function, std::ostream &out) :
    function_(function), out_(out) {}

void FunctionEmitter::emit() {
    buildFrame();
    out_ << "\n" << function_.name << ":\n";
    out_ << "  addiu $sp, $sp, -" << frame_.frameSize << "\n";
    out_ << "  sw $ra, " << frame_.frameSize - 4 << "($sp)\n";
    for (const auto &[reg, offset]: frame_.savedRegs) {
        out_ << "  sw " << reg << ", " << offset << "($sp)\n";
    }
    spillParameters();
    for (const auto &block: function_.blocks) {
        currentBlock_ = block->name;
        out_ << labelOf(block->name) << ":\n";
        for (size_t i = 0; i < block->instructions.size(); ++i) {
            if (canFusePowerOfTwoRemainderBranch(block->instructions, i)) {
                emitPowerOfTwoRemainderBranch(block->instructions[i], block->instructions[i + 1], block->instructions[i + 2]);
                i += 2;
                continue;
            }
            if (canFuseICmpBranch(block->instructions, i)) {
                emitICmpToReg(block->instructions[i], "$t0");
                emitCondBranchWithPhi(block->instructions[i + 1], "$t0");
                ++i;
                continue;
            }
            emitInst(block->instructions[i]);
        }
    }
    if (function_.blocks.empty() || !function_.blocks.back()->terminated()) {
        emitDefaultReturn();
    }
}

int FunctionEmitter::reserveSlot(int bytes) {
    frame_.nextOffset = alignTo(frame_.nextOffset, 4);
    int offset = frame_.nextOffset;
    frame_.nextOffset += alignTo(bytes, 4);
    return offset;
}

void FunctionEmitter::buildFrame() {
    collectUseCounts();
    neededValues_ = collectNeededValues();
    allocateRegisters();
    for (const auto &reg: usedAllocatedRegs()) {
        frame_.savedRegs.push_back({reg, reserveSlot()});
    }
    for (const auto &param: function_.params) {
        auto name = "%" + param.name;
        if (frame_.valueRegs.find(name) == frame_.valueRegs.end()) {
            frame_.valueSlots[name] = reserveSlot();
        }
    }
    for (const auto &block: function_.blocks) {
        frame_.labels[block->name] = sanitizeLabel(function_.name + "_" + block->name);
        for (const auto &inst: block->instructions) {
            if (inst.opcode == IR::Opcode::Alloca) {
                int count = 1;
                if (!inst.operands.empty() && isInteger(inst.operands.front().text)) {
                    count = std::max(1, std::stoi(inst.operands.front().text));
                }
                frame_.pointerSlots[inst.result] = reserveSlot(count * sizeOfIRType(inst.type));
            } else if (inst.hasResult()) {
                if (frame_.valueRegs.find(inst.result) == frame_.valueRegs.end()) {
                    frame_.valueSlots[inst.result] = reserveSlot();
                }
            }
        }
    }
    collectPhiMoves();
    for (const auto &[edge, moves]: phiMoves_) {
        (void) edge;
        frame_.phiTempCount = std::max(frame_.phiTempCount, static_cast<int>(moves.size()));
    }
    if (frame_.phiTempCount > 0) {
        frame_.phiTempOffset = reserveSlot(frame_.phiTempCount * 4);
    }
    frame_.frameSize = alignTo(frame_.nextOffset + 8, 8);
}

std::vector<std::string> FunctionEmitter::usedAllocatedRegs() const {
    std::set<std::string> used;
    for (const auto &[value, reg]: frame_.valueRegs) {
        (void) value;
        used.insert(reg);
    }
    std::vector<std::string> ordered;
    for (const auto &reg: calleeSavedRegs_) {
        if (used.find(reg) != used.end()) {
            ordered.push_back(reg);
        }
    }
    return ordered;
}

void FunctionEmitter::collectUseCounts() {
    useCounts_.clear();
    auto count = [this](const IR::Operand &operand) {
        if (!operand.text.empty() && operand.text.front() == '%') {
            ++useCounts_[operand.text];
        }
    };
    for (const auto &block: function_.blocks) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands) {
                count(operand);
            }
            for (const auto &incoming: inst.incoming) {
                count(incoming.value);
            }
        }
    }
}

void FunctionEmitter::spillParameters() {
    static const char *argRegs[] = {"$a0", "$a1", "$a2", "$a3"};
    for (size_t i = 0; i < function_.params.size() && i < 4; ++i) {
        auto name = "%" + function_.params[i].name;
        auto regIt = frame_.valueRegs.find(name);
        if (regIt != frame_.valueRegs.end()) {
            out_ << "  move " << regIt->second << ", " << argRegs[i] << "\n";
        } else {
            out_ << "  sw " << argRegs[i] << ", " << frame_.valueSlots[name] << "($sp)\n";
        }
    }
    for (size_t i = 4; i < function_.params.size(); ++i) {
        auto name = "%" + function_.params[i].name;
        int callerArgOffset = frame_.frameSize + static_cast<int>((i - 4) * 4);
        auto regIt = frame_.valueRegs.find(name);
        if (regIt != frame_.valueRegs.end()) {
            out_ << "  lw " << regIt->second << ", " << callerArgOffset << "($sp)\n";
        } else {
            out_ << "  lw $t0, " << callerArgOffset << "($sp)\n";
            out_ << "  sw $t0, " << frame_.valueSlots[name] << "($sp)\n";
        }
    }
}

std::string FunctionEmitter::labelOf(const std::string &blockName) const {
    auto it = frame_.labels.find(blockName);
    return it == frame_.labels.end() ? sanitizeLabel(function_.name + "_" + blockName) : it->second;
}

std::string FunctionEmitter::labelFromOperand(const IR::Operand &operand) const {
    return labelOf(labelName(operand));
}

std::string FunctionEmitter::labelName(const IR::Operand &operand) {
    std::string label = operand.text;
    if (!label.empty() && label.front() == '%') {
        label.erase(label.begin());
    }
    return label;
}

void FunctionEmitter::emitDefaultReturn() {
    if (function_.name == "main") {
        out_ << "  li $v0, 10\n";
        out_ << "  syscall\n";
    } else {
        restoreSavedRegs();
        out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
        out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
        out_ << "  jr $ra\n";
    }
}

void FunctionEmitter::restoreSavedRegs() {
    for (const auto &[reg, offset]: frame_.savedRegs) {
        out_ << "  lw " << reg << ", " << offset << "($sp)\n";
    }
}

} // namespace MIPS::detail
