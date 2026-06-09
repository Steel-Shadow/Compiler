//
// Created by Steel_Shadow on 2023/11/15.
//

#include "Instruction.h"

#include <string>
#include <utility>

#include "errorHandler/Error.h"
#include "frontend/symTab/SymTab.h"
#include "Memory.h"
#include "Register.h"


#include <string>

using namespace MIPS;

std::string MIPS::opToString(Op e) {
    switch (e) {
        case Op::none:
            return "none";
        case Op::addu:
            return "addu";
        case Op::subu:
            return "subu";
        case Op::mul:
            return "mul";
        case Op::div:
            return "div";
        case Op::mfhi:
            return "mfhi";
        case Op::and_:
            return "and";
        case Op::or_:
            return "or";
        case Op::sw:
            return "sw";
        case Op::lw:
            return "lw";
        case Op::sb:
            return "sb";
        case Op::lbu:
            return "lbu";
        case Op::li:
            return "li";
        case Op::syscall:
            return "syscall";
        case Op::move:
            return "move";
        case Op::la:
            return "la";
        case Op::j:
            return "j";
        case Op::jal:
            return "jal";
        case Op::addi:
            return "addi";
        case Op::jr:
            return "jr";
        case Op::slt:
            return "slt";
        case Op::sle:
            return "sle";
        case Op::sge:
            return "sge";
        case Op::sgt:
            return "sgt";
        case Op::seq:
            return "seq";
        case Op::sne:
            return "sne";
        case Op::bgtz:
            return "bgtz";
        case Op::beqz:
            return "beqz";
        case Op::add:
            return "add";
        case Op::sll:
            return "sll";
        case Op::bne:
            return "bne";
        case Op::addiu:
            return "addiu";
        case Op::subiu:
            return "subiu";
        case Op::andi:
            return "andi";
        case Op::ori:
            return "ori";
        case Op::slti:
            return "slti";
        default:
            Error::raise("unknown Op");
            return "unknown";
    }
}

Instruction::Instruction(Op op) :
    op(op) {}

R_Inst::R_Inst(Op op, Register rd, Register rs, Register rt) :
    Instruction(op),
    rd(rd),
    rs(rs),
    rt(rt) {}

std::string R_Inst::toString() {
    return opToString(op) + '\t'
           + regToString(rd) + '\t'
           + regToString(rs) + '\t'
           + regToString(rt);
}

I_imm_Inst::I_imm_Inst(Op op, Register rt, Register rs, int immediate) :
    Instruction(op),
    rt(rt),
    rs(rs),
    immediate(immediate) {}

std::string I_imm_Inst::toString() {
    if (op == Op::lw || op == Op::sw || op == Op::lbu || op == Op::sb) {
        return opToString(op) + '\t'
               + regToString(rt) + '\t'
               + std::to_string(immediate)
               + "(" + regToString(rs) + ")";
    } else {
        return opToString(op) + '\t'
               + regToString(rt) + '\t'
               + regToString(rs) + '\t'
               + std::to_string(immediate);
    }
}

I_label_Inst::I_label_Inst(Op op, Register rs, Register rt, Label label, int offset) :
    Instruction(op),
    rs(rs),
    rt(rt),
    label(std::move(label)),
    offset(offset) {}

std::string I_label_Inst::toString() {
    if (rt == Register::none) {
        return opToString(op) + '\t'
               + regToString(rs) + '\t'
               + label.nameAndId
               + (offset == 0 ? "" : " + " + std::to_string(offset));
    } else {
        if (op == Op::bne)
            return opToString(op) + '\t'
                   + regToString(rs) + '\t'
                   + regToString(rt) + '\t'
                   + label.nameAndId;

        return opToString(op) + '\t'
               + regToString(rs) + '\t'
               + label.nameAndId
               + (offset == 0 ? "" : " + " + std::to_string(offset))
               + "(" + regToString(rt) + ")";
    }
}

J_Inst::J_Inst(Op op, Label label) :
    Instruction(op),
    label(std::move(label)) {}

std::string J_Inst::toString() {
    return opToString(op) + '\t'
           + label.nameAndId;
}

Op loadOp(Type type) {
    return ptrToValue(type) == Type::Char ? Op::lbu : Op::lw;
}

Op storeOp(Type type) {
    return ptrToValue(type) == Type::Char ? Op::sb : Op::sw;
}

int elementByteOffset(Type type, int elementOffset) {
    return sizeOfType(ptrToValue(type)) * elementOffset;
}

void truncateChar(const IR::Temp *temp, Register reg) {
    if (temp && temp->type == Type::Char) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::andi, reg, reg, 0xFF));
    }
}

void alignStackToWord() {
    int remainder = StackMemory::curOffset % wordSize;
    if (remainder != 0) {
        StackMemory::curOffset += wordSize - remainder;
    }
}

void loadVarAddress(Register target, const IR::Var *var, const IR::Element *offset) {
    int constOffset = 0;
    const IR::Temp *dynamicOffset = nullptr;
    if (auto constVal = dynamic_cast<const IR::ConstVal *>(offset)) {
        constOffset = elementByteOffset(var->type, constVal->value);
    } else if (auto temp = dynamic_cast<const IR::Temp *>(offset)) {
        dynamicOffset = temp;
    }

    if (var->depth == 0) {
        assemblies.push_back(std::make_unique<I_label_Inst>(Op::la, target, Register::none, Label(var->name), constOffset));
        if (dynamicOffset) {
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, target, target, getReg(dynamicOffset)));
        }
    } else if (var->symType == SymType::Param && !var->dims.empty()) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, target, Register::sp, -getStackOffset(var)));
        if (constOffset != 0) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, target, target, constOffset));
        }
        if (dynamicOffset) {
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, target, target, getReg(dynamicOffset)));
        }
    } else {
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, target, Register::sp, -getStackOffset(var) + constOffset));
        if (dynamicOffset) {
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, target, target, getReg(dynamicOffset)));
        }
    }
}

void MIPS::InStack(const IR::Inst &) {
    StackMemory::offsetStack.push(StackMemory::curOffset);
    curDepth++;
}


void MIPS::OutStack(const IR::Inst &) {
    StackMemory::curOffset = StackMemory::offsetStack.top();
    StackMemory::offsetStack.pop();

    for (auto varReg = varToRegs.begin(); varReg != varToRegs.end();) {
        auto &[var, reg] = *varReg;
        if (var.depth == curDepth) {
            freeVarRegs.push(reg);
            varReg = varToRegs.erase(varReg);
        } else {
            ++varReg;
        }
    }

    curDepth--;
}

void MIPS::Store(const IR::Inst &inst) {
    auto value = dynamic_cast<IR::Temp *>(inst.res.get());
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());

    // const index of array
    int arrayOffset = 0;
    if (inst.arg2) {
        auto arg2 = dynamic_cast<IR::ConstVal *>(inst.arg2.get());
        arrayOffset = elementByteOffset(var->type, arg2->value);
    }
    Op op = storeOp(var->type);

    auto varReg = varToRegs.find(*var);
    if (varReg == varToRegs.end()) {
        if (var->depth == 0) {
            assemblies.push_back(std::make_unique<I_label_Inst>(op, getReg(value), Register::none, Label(var->name), arrayOffset));
        } else {
            if (var->symType == SymType::Param && !var->dims.empty()) {
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(var)));
                assemblies.push_back(std::make_unique<I_imm_Inst>(op, getReg(value), Register::fp, arrayOffset));
            } else {
                assemblies.push_back(std::make_unique<I_imm_Inst>(op, getReg(value), Register::sp, -getStackOffset(var) + arrayOffset));
            }
        }
    } else {
        assemblies.push_back(std::make_unique<R_Inst>(Op::move, varReg->second, getReg(value), Register::none));
    }
}

void MIPS::StoreDynamic(const IR::Inst &inst) {
    auto value = dynamic_cast<IR::Temp *>(inst.res.get());
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());
    auto offset = dynamic_cast<IR::Temp *>(inst.arg2.get());

    if (var->depth == 0) {
        assemblies.push_back(std::make_unique<I_label_Inst>(storeOp(var->type), getReg(value), getReg(offset), Label(var->name)));
    } else {
        if (var->symType == SymType::Param && !var->dims.empty()) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(var)));
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::fp, getReg(offset)));
            assemblies.push_back(std::make_unique<I_imm_Inst>(storeOp(var->type), getReg(value), Register::fp, 0));
        } else {
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::sp, getReg(offset)));
            assemblies.push_back(std::make_unique<I_imm_Inst>(storeOp(var->type), getReg(value), Register::fp, -getStackOffset(var)));
        }
    }
}

void MIPS::Add(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::addu, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Sub(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::subu, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Mul(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::mul, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Div(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::div, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Mod(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    assemblies.push_back(std::make_unique<R_Inst>(Op::div, Register::none, reg1, reg2));
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::mfhi, regRes, Register::none, Register::none));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::And(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::and_, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Or(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::or_, regRes, reg1, reg2));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Neg(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());

    Register reg1 = getReg(arg1);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::subu, regRes, Register::zero, reg1));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Not(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());

    Register reg1 = getReg(arg1);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::seq, regRes, Register::zero, reg1));
    checkTempReg(res, regRes);
}

void MIPS::LoadImd(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto imm = dynamic_cast<IR::ConstVal *>(inst.arg1.get());

    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, regRes, Register::none, imm->value));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::GetInt(const IR::Inst &) {
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 5));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::GetChar(const IR::Inst &) {
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 12));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::GetString(const IR::Inst &inst) {
    auto maxLen = dynamic_cast<IR::Temp *>(inst.res.get());
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());
    loadVarAddress(Register::a0, var, inst.arg2.get());
    assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::a1, getReg(maxLen), Register::none));
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 8));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::PrintInt(const IR::Inst &inst) {
    auto t = dynamic_cast<IR::Temp *>(inst.arg1.get());

    assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::a0, getReg(t), Register::none));
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 1));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::PrintChar(const IR::Inst &inst) {
    auto t = dynamic_cast<IR::Temp *>(inst.arg1.get());

    assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::a0, getReg(t), Register::none));
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 11));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::PrintStr(const IR::Inst &inst) {
    if (auto str = dynamic_cast<IR::Str *>(inst.arg1.get())) {
        assemblies.push_back(std::make_unique<I_label_Inst>(Op::la, Register::a0, Register::none, Label(str->toString())));
    } else if (auto addr = dynamic_cast<IR::Temp *>(inst.arg1.get())) {
        assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::a0, getReg(addr), Register::none));
    } else if (auto var = dynamic_cast<IR::Var *>(inst.arg1.get())) {
        loadVarAddress(Register::a0, var, inst.arg2.get());
    }
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 4));
    assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
}

void MIPS::Alloca(const IR::Inst &inst) {
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());
    auto size = dynamic_cast<IR::ConstVal *>(inst.arg2.get());

    int byte = sizeOfType(var->type) * size->value;

    if (!freeVarRegs.empty() && var->dims.empty() && size->value == 1) {
        // try adding the var to the map
        Register r = freeVarRegs.front();
        freeVarRegs.pop();
        varToRegs[*var] = r;
    } else {
        if (ptrToValue(var->type) != Type::Char) {
            alignStackToWord();
        }
        StackMemory::curOffset += byte;
        StackMemory::varToOffset[*var] = StackMemory::curOffset;
    }
}

void MIPS::Load(const IR::Inst &inst) {
    auto temp = dynamic_cast<IR::Temp *>(inst.res.get());
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());

    Register regRes = newReg(temp);

    auto varReg = varToRegs.find(*var);
    if (varReg == varToRegs.end()) {
        // const index of array
        int arrayOffset = 0;
        if (inst.arg2) {
            auto constOffset = dynamic_cast<IR::ConstVal *>(inst.arg2.get());
            arrayOffset = elementByteOffset(var->type, constOffset->value);
        }

        if (var->depth == 0) {
            assemblies.push_back(std::make_unique<I_label_Inst>(loadOp(var->type), regRes, Register::none, Label(var->name), arrayOffset));
        } else {
            if (var->symType == SymType::Param && !var->dims.empty()) {
                if (!inst.arg2) {
                    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, regRes, Register::sp, -getStackOffset(var)));
                } else {
                    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(var)));
                    assemblies.push_back(std::make_unique<I_imm_Inst>(loadOp(var->type), regRes, Register::fp, arrayOffset));
                }
            } else {
                assemblies.push_back(std::make_unique<I_imm_Inst>(
                        loadOp(var->type), regRes, Register::sp, -getStackOffset(var) + arrayOffset));
            }
        }
    } else {
        assemblies.push_back(std::make_unique<R_Inst>(Op::move, regRes, varReg->second, Register::none));
    }

    checkTempReg(temp, regRes);
}

void MIPS::LoadPtr(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto addr = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto offset = dynamic_cast<IR::ConstVal *>(inst.arg2.get());

    Register regAddr = getReg(addr);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<I_imm_Inst>(loadOp(res->type), regRes, regAddr, offset ? elementByteOffset(res->type, offset->value) : 0));
    checkTempReg(res, regRes);
}

void MIPS::Br(const IR::Inst &inst) {
    auto label = Label(dynamic_cast<IR::Label *>(inst.arg1.get()));
    assemblies.push_back(std::make_unique<J_Inst>(Op::j, label));
}

void MIPS::Bif0(const IR::Inst &inst) {
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto label = Label(dynamic_cast<IR::Label *>(inst.arg2.get()));

    assemblies.push_back(std::make_unique<I_label_Inst>(Op::beqz, getReg(arg1), Register::none, label));
}

void MIPS::Call(const IR::Inst &inst) {
    auto func = Label(dynamic_cast<IR::Label *>(inst.arg1.get()));

    alignStackToWord();
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, Register::sp, Register::sp, -StackMemory::curOffset));
    StackMemory::offsetStack.push(StackMemory::curOffset);
    StackMemory::curOffset = 0;

    // save $sp $ra
    StackMemory::curOffset += wordSize;
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sw, Register::sp, Register::sp, -StackMemory::curOffset));
    StackMemory::curOffset += wordSize;
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sw, Register::ra, Register::sp, -StackMemory::curOffset));

    // save tempRegs
    // Only save used tempRegs, but we still allocate MAX_TEMP_REGS for stack of called function.
    // In this way, we don't need to consider tempRegs after jal.
    // I choose to use more stack memory, but fewer instructions.
    // Another way is, before jal , save the number of used tempRegs to a realReg $?,
    // use the realReg $? to locate parameters instead of $sp. (slower but less memory use of stack)
    int unusedTempRegSlots = MAX_TEMP_REGS;
    for (auto &[tempId, reg]: tempToRegs) {
        StackMemory::curOffset += wordSize;
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, reg, Register::sp, -StackMemory::curOffset));
        --unusedTempRegSlots;
    }

    StackMemory::curOffset += wordSize * unusedTempRegSlots;

    for (auto &[var, reg]: varToRegs) {
        StackMemory::curOffset += wordSize;
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, reg, Register::sp, -StackMemory::curOffset));
    }

    // jal to function body
    assemblies.push_back(std::make_unique<J_Inst>(Op::jal, func));

    // restore varRegs
    for (auto i = varToRegs.rbegin(); i != varToRegs.rend(); ++i) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::lw, i->second, Register::sp, -StackMemory::curOffset));
        StackMemory::curOffset -= wordSize;
    }

    StackMemory::curOffset -= wordSize * unusedTempRegSlots;

    // restore tempRegs
    for (auto i = tempToRegs.rbegin(); i != tempToRegs.rend(); ++i) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::lw, i->second, Register::sp, -StackMemory::curOffset));
        StackMemory::curOffset -= wordSize;
    }

    // restore $ra $sp
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::ra, Register::sp, -StackMemory::curOffset));
    StackMemory::curOffset -= wordSize;
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::sp, Register::sp, -StackMemory::curOffset));
    StackMemory::curOffset -= wordSize;

    // StackMemory::curOffset -= SymTab::find(func.nameAndId)->params.size() * wordSize;
    // deAllocate stack for call
    StackMemory::curOffset = StackMemory::offsetStack.top();
    StackMemory::offsetStack.pop();
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, Register::sp, Register::sp, StackMemory::curOffset));
}

void MIPS::PushParam(const IR::Inst &inst) {
    auto param = dynamic_cast<IR::Temp *>(inst.arg1.get());

    // set function's parameters to varToOffset is done in MIPS.cpp
    alignStackToWord();
    StackMemory::curOffset += wordSize;
    assemblies.push_back(std::make_unique<I_imm_Inst>(storeOp(param->type), getReg(param), Register::sp, -StackMemory::curOffset));
}

void MIPS::PushAddressParam(const IR::Inst &inst) {
    auto varAddr = dynamic_cast<IR::Var *>(inst.arg1.get());

    // set function's parameters to varToOffset is done in MIPS.cpp
    if (varAddr->depth == 0) {
        if (auto constOffset = dynamic_cast<IR::ConstVal *>(inst.arg2.get())) {
            int offset = elementByteOffset(varAddr->type, constOffset->value);
            assemblies.push_back(std::make_unique<I_label_Inst>(Op::la, Register::fp, Register::none, Label(varAddr->name), offset));
        } else {
            auto dynamicOffset = dynamic_cast<IR::Temp *>(inst.arg2.get());
            assemblies.push_back(std::make_unique<I_label_Inst>(Op::la, Register::fp, getReg(dynamicOffset), Label(varAddr->name)));
        }
    } else {
        if (auto constOffset = dynamic_cast<IR::ConstVal *>(inst.arg2.get())) {
            // int offset = constOffset->value * wordSize - getStackOffset(varAddr);
            if (varAddr->symType == SymType::Param && !varAddr->dims.empty()) {
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(varAddr)));
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, Register::fp, Register::fp, elementByteOffset(varAddr->type, constOffset->value)));
            } else {
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, Register::fp, Register::sp,
                                                                  elementByteOffset(varAddr->type, constOffset->value) - getStackOffset(varAddr)));
            }
        } else {
            if (varAddr->symType == SymType::Param && !varAddr->dims.empty()) {
                auto dynamicOffset = dynamic_cast<IR::Temp *>(inst.arg2.get());
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(varAddr)));
                assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::fp, getReg(dynamicOffset)));
            } else {
                auto dynamicOffset = dynamic_cast<IR::Temp *>(inst.arg2.get());
                assemblies.push_back(std::make_unique<I_imm_Inst>(Op::addiu, Register::fp, Register::sp, -getStackOffset(varAddr)));
                assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::fp, getReg(dynamicOffset)));
            }
        }
    }

    alignStackToWord();
    StackMemory::curOffset += wordSize;
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sw, Register::fp, Register::sp, -StackMemory::curOffset));
}

void MIPS::Ret(const IR::Inst &inst) {
    auto ret = dynamic_cast<IR::Temp *>(inst.arg1.get());
    if (ret) {
        assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::v0, getReg(ret), Register::none));
    }
    assemblies.push_back(std::make_unique<R_Inst>(Op::jr, Register::none, Register::ra, Register::none));
}

void MIPS::RetMain(const IR::Inst &inst) {
    auto ret = dynamic_cast<IR::Temp *>(inst.arg1.get());
    if (ret) {
        assemblies.push_back(std::make_unique<R_Inst>(Op::move, Register::a0, getReg(ret), Register::none));
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 17));
        assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
    } else {
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::li, Register::v0, Register::none, 10));
        assemblies.push_back(std::make_unique<R_Inst>(Op::syscall, Register::none, Register::none, Register::none));
    }
}

void MIPS::NewMove(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());

    Register regRes = newReg(res);
    Register reg1 = getReg(arg1);
    assemblies.push_back(std::make_unique<R_Inst>(Op::move, regRes, reg1, Register::none));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Leq(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::sle, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Lss(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::slt, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Geq(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::sge, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Gre(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::sgt, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Eql(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::seq, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Neq(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto arg2 = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register reg1 = getReg(arg1);
    Register reg2 = getReg(arg2);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<R_Inst>(Op::sne, regRes, reg1, reg2));
    checkTempReg(res, regRes);
}

void MIPS::Bif1(const IR::Inst &inst) {
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto label = Label(dynamic_cast<IR::Label *>(inst.arg2.get()));

    assemblies.push_back(std::make_unique<I_label_Inst>(Op::bne, getReg(arg1), Register::zero, label));
}

void MIPS::LoadDynamic(const IR::Inst &inst) {
    auto value = dynamic_cast<IR::Temp *>(inst.res.get());
    auto var = dynamic_cast<IR::Var *>(inst.arg1.get());
    auto offset = dynamic_cast<IR::Temp *>(inst.arg2.get());

    Register regOffset = getReg(offset);
    Register regValue = newReg(value);
    if (var->depth == 0) {
        assemblies.push_back(std::make_unique<I_label_Inst>(loadOp(var->type), regValue, regOffset, Label(var->name)));
    } else {
        if (var->symType == SymType::Param && !var->dims.empty()) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::fp, Register::sp, -getStackOffset(var)));
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::fp, regOffset));
            assemblies.push_back(std::make_unique<I_imm_Inst>(loadOp(var->type), regValue, Register::fp, 0));
        } else {
            assemblies.push_back(std::make_unique<R_Inst>(Op::addu, Register::fp, Register::sp, regOffset));
            assemblies.push_back(std::make_unique<I_imm_Inst>(loadOp(var->type), regValue, Register::fp, -getStackOffset(var)));
        }
    }
    checkTempReg(value, regValue);
}

void MIPS::MulImd(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());
    auto imm = dynamic_cast<IR::ConstVal *>(inst.arg2.get());

    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::mul, regRes, getReg(arg1), imm->value));
    truncateChar(res, regRes);
    checkTempReg(res, regRes);
}

void MIPS::Mult4(const IR::Inst &inst) {
    auto res = dynamic_cast<IR::Temp *>(inst.res.get());
    auto arg1 = dynamic_cast<IR::Temp *>(inst.arg1.get());

    Register reg1 = getReg(arg1);
    Register regRes = newReg(res);
    assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sll, regRes, reg1, 2));
    checkTempReg(res, regRes);
}
