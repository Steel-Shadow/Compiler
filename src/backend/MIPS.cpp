//
// Created by Steel_Shadow on 2023/10/31.
//
#include "MIPS.h"

#include <utility>

#include "config.h"
#include "errorHandler/Error.h"
#include "Instruction.h"
#include "Memory.h"


#include <string>

using namespace MIPS;

int MIPS::curDepth = 1;

std::ofstream MIPS::mipsFileStream;
std::vector<std::unique_ptr<Assembly>> MIPS::assemblies; // maybe use List is faster in optimization

void MIPS::output(const std::string &str, bool newLine) {
#if defined(FILEOUT_MIPS)
    mipsFileStream << str;
    if (newLine) {
        mipsFileStream << '\n';
    }
#endif
#if defined(STDOUT_MIPS)
    std::cout << str;
    if (newLine) {
        std::cout << '\n';
    }
#endif
}

Label::Label(std::string name_and_id) :
    nameAndId(std::move(name_and_id)) {}

Label::Label(const IR::Label *label) :
    nameAndId(label->nameAndId) {}

std::string Label::toString() {
    return nameAndId + ":";
}

void MIPS::genMIPS(const IR::Module &module) {
    /*---- .data generate & output ----------------------*/
    output("#### MIPS ####");
    output(".data");
    for (auto &[name, globVar]: module.getGlobVars()) {
        std::string dataLine = name + (globVar.type == Type::Char ? ": .byte " : ": .word ");
        for (size_t i = 0; i < globVar.initVal.size(); ++i) {
            if (i != 0) {
                dataLine += ", ";
            }
            dataLine += std::to_string(globVar.initVal[i]);
        }
        output(dataLine);
    }
    int i = 0;
    for (const auto &str: IR::Str::MIPS_strings) {
        output("str_" + std::to_string(i) + ": .asciiz " + str);
        i++;
    }

    /*----- .text generate ---------------------*/
    output("");
    output(".text");
    // main
    for (auto &basicBlock: module.getMainFunction().getBasicBlocks()) {
        assemblies.push_back(std::make_unique<Label>(basicBlock->label.nameAndId));
        for (auto &inst: basicBlock->instructions) {
            irToMips(inst);
        }
    }

    // other function
    for (auto &func: module.getFunctions()) {
        clearRegs();
        // Use part of tempRegs, but move stackOffset for MAX_TEMP_REGS.
        StackMemory::curOffset = wordSize * (2 + MAX_TEMP_REGS + MAX_VAR_REGS);
        StackMemory::varToOffset.clear();

        // set function's parameters to varToOffset
        // stack memory map explain is in markdown and Memory.h
        int offset = 0;
        for (auto &[ident, sym]: func->getParams()) {
            StackMemory::varToOffset.emplace(IR::Var(ident, 1, false, sym->dims, sym->type), -offset);
            offset += wordSize;
        }

        for (auto &basicBlock: func->getBasicBlocks()) {
            assemblies.push_back(std::make_unique<Label>(basicBlock->label.nameAndId));
            for (auto &inst: basicBlock->instructions) {
                irToMips(inst);
            }
        }
    }

    /*----- .text optimize ---------------------*/
    while (allMergeR_Move()) {}
    while (allMergeMove_R_rs()) {}
    while (allMergeLi_Move()) {}
    while (allMergeMove_R_rt()) {}
    while (allMergeLi_R()) {}

    /*----- .text output  ---------------------*/
    for (auto &assem: assemblies) {
        output(assem->toString());
    }
}

Op rOp_ImmOp(Op rOp) {
    switch (rOp) {
        case Op::addu:
            return Op::addiu;
        case Op::subu:
            return Op::subiu;
        case Op::and_:
            return Op::andi;
        case Op::or_:
            return Op::ori;
        case Op::add:
            return Op::addi;
        case Op::slt:
            return Op::slti;
        default:
            return Op::none;
    }
}

std::unique_ptr<I_imm_Inst> MIPS::mergeLi_R(const I_imm_Inst &li, const R_Inst &r) {
    // li   $t1 1
    // addu $t2 $t0 $t1
    // ------------------
    // addiu $t2 $t0 1
    return std::make_unique<I_imm_Inst>(rOp_ImmOp(r.op), r.rd, r.rs, li.immediate);
}

namespace {

bool validReg(Register reg) {
    return reg != Register::none;
}

bool readsReg(const R_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::move:
        case Op::jr:
            return inst.rs == reg;
        case Op::mfhi:
        case Op::none:
            return false;
        case Op::syscall:
            return reg == Register::v0 || reg == Register::a0 || reg == Register::a1;
        default:
            return inst.rs == reg || inst.rt == reg;
    }
}

bool writesReg(const R_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::addu:
        case Op::subu:
        case Op::mul:
        case Op::div:
        case Op::mfhi:
        case Op::and_:
        case Op::or_:
        case Op::add:
        case Op::slt:
        case Op::sle:
        case Op::sge:
        case Op::sgt:
        case Op::seq:
        case Op::sne:
        case Op::move:
            return inst.rd == reg;
        case Op::syscall:
            return reg == Register::v0;
        default:
            return false;
    }
}

bool readsReg(const I_imm_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::li:
            return false;
        case Op::sw:
        case Op::sb:
            return inst.rt == reg || inst.rs == reg;
        default:
            return inst.rs == reg;
    }
}

bool writesReg(const I_imm_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::sw:
        case Op::sb:
            return false;
        default:
            return inst.rt == reg;
    }
}

bool readsReg(const I_label_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::bne:
            return inst.rs == reg || inst.rt == reg;
        case Op::beqz:
        case Op::bgtz:
            return inst.rs == reg;
        case Op::sw:
        case Op::sb:
            return inst.rs == reg || inst.rt == reg;
        case Op::lw:
        case Op::lbu:
        case Op::la:
            return inst.rt == reg;
        default:
            return false;
    }
}

bool writesReg(const I_label_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::lw:
        case Op::lbu:
        case Op::la:
            return inst.rs == reg;
        default:
            return false;
    }
}

bool readsReg(const Assembly *assembly, Register reg) {
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_imm_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    return false;
}

bool writesReg(const Assembly *assembly, Register reg) {
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_imm_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const J_Inst *>(assembly)) {
        return inst->op == Op::jal && reg == Register::ra;
    }
    return false;
}

bool isControlBoundary(const Assembly *assembly) {
    if (dynamic_cast<const Label *>(assembly)) {
        return true;
    }
    if (dynamic_cast<const J_Inst *>(assembly)) {
        return true;
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return inst->op == Op::bne || inst->op == Op::beqz || inst->op == Op::bgtz;
    }
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return inst->op == Op::jr;
    }
    return false;
}

bool isReadBeforeOverwrite(size_t start, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    for (size_t i = start; i < assemblies.size(); ++i) {
        const Assembly *assembly = assemblies[i].get();
        if (readsReg(assembly, reg)) {
            return true;
        }
        if (writesReg(assembly, reg)) {
            return false;
        }
        if (isControlBoundary(assembly)) {
            return true;
        }
    }
    return false;
}

bool mergeMoveIntoNextR(size_t index, bool requireRsUse, bool requireRtUse) {
    auto move = dynamic_cast<R_Inst *>(assemblies[index].get());
    auto cal = dynamic_cast<R_Inst *>(assemblies[index + 1].get());
    if (!move || !cal || move->op != Op::move) {
        return false;
    }
    if (requireRsUse && cal->rs != move->rd) {
        return false;
    }
    if (requireRtUse && cal->rt != move->rd) {
        return false;
    }

    const bool used = cal->rs == move->rd || cal->rt == move->rd;
    if (!used) {
        return false;
    }
    if (!writesReg(*cal, move->rd) && isReadBeforeOverwrite(index + 2, move->rd)) {
        return false;
    }

    if (cal->rs == move->rd) {
        cal->rs = move->rs;
    }
    if (cal->rt == move->rd) {
        cal->rt = move->rs;
    }

    assemblies.erase(assemblies.begin() + index);
    return true;
}

} // namespace

// merge immediate instructions
bool MIPS::allMergeLi_R() {
    // li   $t1 1
    // addu $t2 $t0 $t1
    // ------------------
    // addiu $t2 $t0 1
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto r = dynamic_cast<R_Inst *>(inst2);
            if (!li || !r) {
                ++i;
                continue;
            }

            constexpr int MAX_16_BIT = (1 << 15) - 1;
            constexpr int NEG_MIN_16_BIT = -(1 << 15);
            if (li->immediate > MAX_16_BIT || li->immediate < NEG_MIN_16_BIT) {
                ++i;
                continue;
            }

            if (r->rt == li->rt && r->rs != li->rt && rOp_ImmOp(r->op) != Op::none
                && (r->rd == li->rt || !isReadBeforeOverwrite(i + 2, li->rt))) {
                *assem2 = mergeLi_R(*li, *r);
                assemblies.erase(assem1);
                flag = true;
                continue;
            }
        }
        ++i;
    }
    return flag;
}

bool MIPS::allMergeLi_Move() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2 && inst2->op == Op::move) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto move = dynamic_cast<R_Inst *>(inst2);
            if (li && move && li->rt == move->rs
                && (move->rd == li->rt || !isReadBeforeOverwrite(i + 2, li->rt))) {
                li->rt = move->rd;
                assemblies.erase(assem2);
                flag = true;
                continue;
            }
        }
        ++i;
    }

    return flag;
}

// Load
bool MIPS::allMergeMove_R_rs() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        if (mergeMoveIntoNextR(i, true, false)) {
            flag = true;
            continue;
        }
        ++i;
    }

    return flag;
}

bool MIPS::allMergeMove_R_rt() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        if (mergeMoveIntoNextR(i, false, true)) {
            flag = true;
            continue;
        }
        ++i;
    }

    return flag;
}

bool MIPS::allMergeR_Move() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto cal = dynamic_cast<R_Inst *>(assem1->get());
        auto move = dynamic_cast<R_Inst *>(assem2->get());

        if (cal && move && move->op == Op::move && cal->op != Op::move
            && writesReg(*cal, cal->rd) && cal->rd == move->rs
            && (move->rd == cal->rd || !isReadBeforeOverwrite(i + 2, cal->rd))) {
            cal->rd = move->rd;
            assemblies.erase(assem2);
            flag = true;
            continue;
        }
        ++i;
    }

    return flag;
}

void MIPS::irToMips(const IR::Inst &inst) {
    switch (inst.op) {
        case IR::Op::Empty:
            break;
        case IR::Op::InStack:
            InStack(inst);
            break;
        case IR::Op::OutStack:
            OutStack(inst);
            break;
        case IR::Op::Store:
            Store(inst);
            break;
        case IR::Op::StoreDynamic:
            StoreDynamic(inst);
            break;
        case IR::Op::Add:
            Add(inst);
            break;
        case IR::Op::Sub:
            Sub(inst);
            break;
        case IR::Op::Mul:
            Mul(inst);
            break;
        case IR::Op::Div:
            Div(inst);
            break;
        case IR::Op::Mod:
            Mod(inst);
            break;
        case IR::Op::And:
            And(inst);
            break;
        case IR::Op::Or:
            Or(inst);
            break;
        case IR::Op::Neg:
            Neg(inst);
            break;
        case IR::Op::LoadImd:
            LoadImd(inst);
            break;
        case IR::Op::GetInt:
            GetInt(inst);
            break;
        case IR::Op::GetChar:
            GetChar(inst);
            break;
        case IR::Op::GetString:
            GetString(inst);
            break;
        case IR::Op::PrintInt:
            PrintInt(inst);
            break;
        case IR::Op::PrintChar:
            PrintChar(inst);
            break;
        case IR::Op::PrintStr:
            PrintStr(inst);
            break;
        case IR::Op::Alloca:
            Alloca(inst);
            break;
        case IR::Op::Load:
            Load(inst);
            break;
        case IR::Op::LoadPtr:
            LoadPtr(inst);
            break;
        case IR::Op::Br:
            Br(inst);
            break;
        case IR::Op::Bif0:
            Bif0(inst);
            break;
        case IR::Op::Call:
            Call(inst);
            break;
        case IR::Op::PushParam:
            PushParam(inst);
            break;
        case IR::Op::Ret:
            Ret(inst);
            break;
        case IR::Op::RetMain:
            RetMain(inst);
            break;
        case IR::Op::NewMove:
            NewMove(inst);
            break;
        case IR::Op::Leq:
            Leq(inst);
            break;
        case IR::Op::Lss:
            Lss(inst);
            break;
        case IR::Op::Geq:
            Geq(inst);
            break;
        case IR::Op::Gre:
            Gre(inst);
            break;
        case IR::Op::Eql:
            Eql(inst);
            break;
        case IR::Op::Neq:
            Neq(inst);
            break;
        case IR::Op::Bif1:
            Bif1(inst);
            break;
        case IR::Op::LoadDynamic:
            LoadDynamic(inst);
            break;
        case IR::Op::MulImd:
            MulImd(inst);
            break;
        case IR::Op::Mult4:
            Mult4(inst);
            break;
        case IR::Op::PushAddressParam:
            PushAddressParam(inst);
            break;
        case IR::Op::Not:
            Not(inst);
            break;
        default:
            Error::raise("Bad IR Op in MIPS gen");
    }
}
