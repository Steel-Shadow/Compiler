#include "backend/MIPSInternal.h"

#include <algorithm>

namespace MIPS::detail {

void FunctionEmitter::emitInst(const IR::Instruction &inst) {
    switch (inst.opcode) {
        case IR::Opcode::Alloca:
            break;
        case IR::Opcode::Load:
            emitLoad(inst);
            break;
        case IR::Opcode::Store:
            emitStore(inst);
            break;
        case IR::Opcode::Binary:
            emitBinary(inst);
            break;
        case IR::Opcode::ICmp:
            emitICmp(inst);
            break;
        case IR::Opcode::Br:
            emitPhiMoves(labelName(inst.operands.front()));
            out_ << "  j " << labelFromOperand(inst.operands.front()) << "\n";
            break;
        case IR::Opcode::CondBr:
            emitCondBranchWithPhi(inst, materializeOperand(inst.operands[0], "$t0"));
            break;
        case IR::Opcode::Ret:
            emitReturn(inst);
            break;
        case IR::Opcode::Call:
            emitCall(inst);
            break;
        case IR::Opcode::Phi:
            break;
        case IR::Opcode::GetElementPtr:
            emitGetElementPtr(inst);
            break;
        case IR::Opcode::Cast:
            emitCast(inst);
            break;
        case IR::Opcode::Comment:
            out_ << "  # " << inst.note << "\n";
            break;
    }
}

bool FunctionEmitter::canFuseICmpBranch(const std::vector<IR::Instruction> &instructions, size_t index) const {
    if (index + 1 >= instructions.size()) {
        return false;
    }
    const auto &cmp = instructions[index];
    const auto &branch = instructions[index + 1];
    if (cmp.opcode != IR::Opcode::ICmp || branch.opcode != IR::Opcode::CondBr || branch.operands.empty()) {
        return false;
    }
    if (branch.operands[0].text != cmp.result) {
        return false;
    }
    auto it = useCounts_.find(cmp.result);
    return it != useCounts_.end() && it->second == 1;
}

bool FunctionEmitter::canFusePowerOfTwoRemainderBranch(const std::vector<IR::Instruction> &instructions, size_t index) const {
    if (index + 2 >= instructions.size()) {
        return false;
    }
    const auto &rem = instructions[index];
    const auto &cmp = instructions[index + 1];
    const auto &branch = instructions[index + 2];
    if (rem.opcode != IR::Opcode::Binary || rem.op != "srem" || cmp.opcode != IR::Opcode::ICmp ||
        branch.opcode != IR::Opcode::CondBr) {
        return false;
    }
    if (cmp.op != "eq" && cmp.op != "ne") {
        return false;
    }
    if (branch.operands.empty() || branch.operands[0].text != cmp.result) {
        return false;
    }
    if (useCounts_.find(rem.result) == useCounts_.end() || useCounts_.at(rem.result) != 1 ||
        useCounts_.find(cmp.result) == useCounts_.end() || useCounts_.at(cmp.result) != 1) {
        return false;
    }
    if (rem.operands.size() < 2 || !isInteger(rem.operands[1].text)) {
        return false;
    }
    int divisor = std::stoi(rem.operands[1].text);
    if (divisor <= 0 || (divisor & (divisor - 1)) != 0) {
        return false;
    }
    bool remIsLhs = cmp.operands[0].text == rem.result && isInteger(cmp.operands[1].text) && std::stoi(cmp.operands[1].text) == 0;
    bool remIsRhs = cmp.operands[1].text == rem.result && isInteger(cmp.operands[0].text) && std::stoi(cmp.operands[0].text) == 0;
    return remIsLhs || remIsRhs;
}

void FunctionEmitter::emitLoad(const IR::Instruction &inst) {
    const auto &ptr = inst.operands.front();
    std::string dest = resultRegister(inst.result, "$t0");
    if (!ptr.text.empty() && ptr.text.front() == '@') {
        out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " " << dest << ", " << stripPrefix(ptr.text) << "\n";
    } else if (frame_.pointerSlots.find(ptr.text) != frame_.pointerSlots.end()) {
        auto it = frame_.pointerSlots.find(ptr.text);
        out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " " << dest << ", " << it->second << "($sp)\n";
    } else {
        std::string addr = materializePointerAddress(ptr, "$t9");
        out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " " << dest << ", 0(" << addr << ")\n";
    }
    storeValue(inst.result, dest);
}

void FunctionEmitter::emitStore(const IR::Instruction &inst) {
    std::string value = materializeOperand(inst.operands[0], "$t0");
    const auto &ptr = inst.operands[1];
    if (!ptr.text.empty() && ptr.text.front() == '@') {
        out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " " << value << ", " << stripPrefix(ptr.text) << "\n";
    } else if (frame_.pointerSlots.find(ptr.text) != frame_.pointerSlots.end()) {
        auto it = frame_.pointerSlots.find(ptr.text);
        out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " " << value << ", " << it->second << "($sp)\n";
    } else {
        std::string addr = materializePointerAddress(ptr, "$t9");
        out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " " << value << ", 0(" << addr << ")\n";
    }
}

void FunctionEmitter::emitGetElementPtr(const IR::Instruction &inst) {
    std::string base = materializePointerAddress(inst.operands[0], "$t0");
    std::string index = materializeOperand(inst.operands[1], "$t1");
    if (inst.note == "i32") {
        out_ << "  sll $t1, " << index << ", 2\n";
        index = "$t1";
    }
    std::string dest = resultRegister(inst.result, "$t2");
    out_ << "  addu " << dest << ", " << base << ", " << index << "\n";
    storeValue(inst.result, dest);
}

void FunctionEmitter::emitBinary(const IR::Instruction &inst) {
    std::string lhs = materializeOperand(inst.operands[0], "$t0");
    std::string rhs = materializeOperand(inst.operands[1], "$t1");
    std::string dest = resultRegister(inst.result, "$t2");
    if (inst.op == "add") {
        out_ << "  addu " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "sub") {
        out_ << "  subu " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "mul") {
        out_ << "  mul " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "sdiv") {
        out_ << "  div " << lhs << ", " << rhs << "\n";
        out_ << "  mflo " << dest << "\n";
    } else if (inst.op == "srem") {
        out_ << "  div " << lhs << ", " << rhs << "\n";
        out_ << "  mfhi " << dest << "\n";
    } else if (inst.op == "and") {
        out_ << "  and " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "or") {
        out_ << "  or " << dest << ", " << lhs << ", " << rhs << "\n";
    } else {
        out_ << "  # unsupported binary op " << inst.op << "\n";
        out_ << "  move " << dest << ", $zero\n";
    }
    storeValue(inst.result, dest);
}

void FunctionEmitter::emitICmp(const IR::Instruction &inst) {
    std::string dest = resultRegister(inst.result, "$t2");
    emitICmpToReg(inst, dest);
    storeValue(inst.result, dest);
}

void FunctionEmitter::emitICmpToReg(const IR::Instruction &inst, const std::string &dest) {
    std::string lhs = materializeOperand(inst.operands[0], "$t0");
    std::string rhs = materializeOperand(inst.operands[1], "$t1");
    if (inst.op == "slt") {
        out_ << "  slt " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "sgt") {
        out_ << "  slt " << dest << ", " << rhs << ", " << lhs << "\n";
    } else if (inst.op == "sle") {
        out_ << "  slt " << dest << ", " << rhs << ", " << lhs << "\n";
        out_ << "  xori " << dest << ", " << dest << ", 1\n";
    } else if (inst.op == "sge") {
        out_ << "  slt " << dest << ", " << lhs << ", " << rhs << "\n";
        out_ << "  xori " << dest << ", " << dest << ", 1\n";
    } else if (inst.op == "eq") {
        out_ << "  seq " << dest << ", " << lhs << ", " << rhs << "\n";
    } else if (inst.op == "ne") {
        out_ << "  sne " << dest << ", " << lhs << ", " << rhs << "\n";
    } else {
        out_ << "  # unsupported icmp " << inst.op << "\n";
        out_ << "  move " << dest << ", $zero\n";
    }
}

void FunctionEmitter::emitPowerOfTwoRemainderBranch(const IR::Instruction &rem,
                                   const IR::Instruction &cmp,
                                   const IR::Instruction &branch) {
    int divisor = std::stoi(rem.operands[1].text);
    std::string value = materializeOperand(rem.operands[0], "$t0");
    out_ << "  andi $t0, " << value << ", " << (divisor - 1) << "\n";
    if (cmp.op == "eq") {
        out_ << "  seq $t0, $t0, $zero\n";
    } else {
        out_ << "  sne $t0, $t0, $zero\n";
    }
    emitCondBranchWithPhi(branch);
}

void FunctionEmitter::emitCast(const IR::Instruction &inst) {
    std::string source = materializeOperand(inst.operands.front(), "$t0");
    std::string dest = resultRegister(inst.result, "$t0");
    if (inst.op == "trunc") {
        out_ << "  andi " << dest << ", " << source << ", 255\n";
    } else if (dest != source) {
        out_ << "  move " << dest << ", " << source << "\n";
    }
    storeValue(inst.result, dest);
}

void FunctionEmitter::emitCall(const IR::Instruction &inst) {
    if (inst.op == "get_int") {
        out_ << "  li $v0, 5\n";
        out_ << "  syscall\n";
        storeValue(inst.result, "$v0");
        return;
    }
    if (inst.op == "get_char") {
        out_ << "  li $v0, 12\n";
        out_ << "  syscall\n";
        storeValue(inst.result, "$v0");
        return;
    }
    if (inst.op == "put_int" || inst.op == "put_char") {
        if (!inst.operands.empty()) {
            loadOperand(inst.operands.front(), "$a0");
        }
        out_ << "  li $v0, " << (inst.op == "put_int" ? 1 : 11) << "\n";
        out_ << "  syscall\n";
        return;
    }
    if (inst.op == "put_string" || inst.op == "put_str") {
        if (!inst.operands.empty()) {
            loadOperand(inst.operands.front(), "$a0");
        }
        out_ << "  li $v0, 4\n";
        out_ << "  syscall\n";
        return;
    }
    if (inst.op == "get_string") {
        if (!inst.operands.empty()) {
            loadOperand(inst.operands[0], "$a0");
        }
        if (inst.operands.size() > 1) {
            loadOperand(inst.operands[1], "$a1");
        }
        out_ << "  li $v0, 8\n";
        out_ << "  syscall\n";
        return;
    }

    static const char *argRegs[] = {"$a0", "$a1", "$a2", "$a3"};
    for (size_t i = 0; i < inst.operands.size() && i < 4; ++i) {
        loadOperand(inst.operands[i], argRegs[i]);
    }
    int extraCount = inst.operands.size() > 4 ? static_cast<int>(inst.operands.size() - 4) : 0;
    int extraBytes = extraCount * 4;
    for (size_t i = 4; i < inst.operands.size(); ++i) {
        loadOperand(inst.operands[i], "$t0");
        int offset = -extraBytes + static_cast<int>((i - 4) * 4);
        out_ << "  sw $t0, " << offset << "($sp)\n";
    }
    if (extraBytes > 0) {
        out_ << "  addiu $sp, $sp, -" << extraBytes << "\n";
    }
    out_ << "  jal " << inst.op << "\n";
    if (extraBytes > 0) {
        out_ << "  addiu $sp, $sp, " << extraBytes << "\n";
    }
    if (!inst.result.empty()) {
        storeValue(inst.result, "$v0");
    }
}

void FunctionEmitter::emitReturn(const IR::Instruction &inst) {
    if (function_.name == "main") {
        if (!inst.operands.empty()) {
            loadOperand(inst.operands.front(), "$a0");
            out_ << "  li $v0, 17\n";
        } else {
            out_ << "  li $v0, 10\n";
        }
        out_ << "  syscall\n";
        return;
    }
    if (!inst.operands.empty()) {
        loadOperand(inst.operands.front(), "$v0");
    }
    restoreSavedRegs();
    out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
    out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
    out_ << "  jr $ra\n";
}

} // namespace MIPS::detail
