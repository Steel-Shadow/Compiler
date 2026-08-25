//
// Created by Steel_Shadow on 2023/11/15.
//

#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include "MIPS.h"
#include "Register.h"

namespace MIPS {
enum class Op {
    // IR::Instruction.op == Empty
    // it's not nop
    none,

    addu,
    subu,
    mul,
    div,
    mfhi,
    and_,
    or_,
    addi,

    add,

    slt,
    sle,
    sge,
    sgt,
    seq,
    sne,

    move,
    sw,
    lw,
    sb,
    lbu,
    li,
    la,
    syscall,
    j,
    jal,
    jr,
    bgtz,
    beqz,
    beq,
    sll,
    srl,
    sra,
    srav,
    clz,
    bne,
    blt,
    ble,
    bge,
    bgt,
    bltu,

    addiu,
    subiu,
    andi,
    ori,
    slti,
};

std::string opToString(Op e);
Op loadOp(Type type);
Op storeOp(Type type);

struct Instruction : public Assembly {
    Op op;

    explicit Instruction(Op op = Op::none);
};

struct R_Inst : public Instruction {
    Register rd;
    Register rs;
    Register rt;

    R_Inst(Op op, Register rd, Register rs, Register rt);

    std::string toString() override;
};

struct I_imm_Inst : public Instruction {
    Register rt;
    Register rs;
    int immediate;

    I_imm_Inst(Op op, Register rt, Register rs, int immediate);

    std::string toString() override;
};

struct I_label_Inst : public Instruction {
    Register rs;
    Register rt;
    Label label;
    int offset;

    I_label_Inst(Op op, Register rs, Register rt, Label label, int offset = 0);

    std::string toString() override;
};

struct J_Inst : public Instruction {
    Label label;

    explicit J_Inst(Op op, Label label);

    std::string toString() override;
};

void InStack(const IR::Inst &);
void OutStack(const IR::Inst &);
void Store(const IR::Inst &);
void StoreDynamic(const IR::Inst &);
void Add(const IR::Inst &);
void Sub(const IR::Inst &);
void Mul(const IR::Inst &);
void Div(const IR::Inst &);
void Mod(const IR::Inst &);
void And(const IR::Inst &);
void Or(const IR::Inst &);
void Neg(const IR::Inst &);
void Not(const IR::Inst &);
void LoadImd(const IR::Inst &);
void GetInt(const IR::Inst &);
void GetChar(const IR::Inst &);
void GetString(const IR::Inst &);
void PrintInt(const IR::Inst &);
void PrintChar(const IR::Inst &);
void PrintStr(const IR::Inst &);
void Alloca(const IR::Inst &);
void Load(const IR::Inst &);
void LoadPtr(const IR::Inst &);
void Br(const IR::Inst &);
void Bif0(const IR::Inst &);
void Call(const IR::Inst &);
void PushParam(const IR::Inst &);
void PushAddressParam(const IR::Inst &);
void Ret(const IR::Inst &);
void RetMain(const IR::Inst &);
void NewMove(const IR::Inst &);
void Leq(const IR::Inst &);
void Lss(const IR::Inst &);
void Geq(const IR::Inst &);
void Gre(const IR::Inst &);
void Eql(const IR::Inst &);
void Neq(const IR::Inst &);
void Bif1(const IR::Inst &);
void LoadDynamic(const IR::Inst &);
void MulImd(const IR::Inst &);
void Mult4(const IR::Inst &);
#endif
}
