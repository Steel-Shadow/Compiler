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
        for (int i = 0; i < static_cast<int>(globVar.initVal.size()); ++i) {
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
    // TODO: these two cause bugs!
    // while (allMergeR_Move()) {}
    // while (allMergeMove_R_rs()) {}
    // while (allMergeLi_Move()) {}


    // good optimize here
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

// merge immediate instructions
bool MIPS::allMergeLi_R() {
    // li   $t1 1
    // addu $t2 $t0 $t1
    // ------------------
    // addiu $t2 $t0 1
    bool flag = false;
    for (auto assem1 = assemblies.begin(); assem1 != assemblies.end() - 1; ++assem1) {
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto r = dynamic_cast<R_Inst *>(inst2);

            constexpr int MAX_16_BIT = (1 << 15) - 1;
            constexpr int NEG_MIN_16_BIT = -(1 << 15);
            if (li->immediate > MAX_16_BIT || li->immediate < NEG_MIN_16_BIT) {
                continue;
            }

            if (r && r->rt == li->rt && rOp_ImmOp(r->op) != Op::none) {
                *assem2 = mergeLi_R(*li, *r);
                assem1 = assemblies.erase(assem1);
                flag = true;
            }
        }
    }
    return flag;
}

bool MIPS::allMergeLi_Move() {
    bool flag = false;
    for (auto assem1 = assemblies.begin(); assem1 != assemblies.end() - 1; ++assem1) {
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2 && inst2->op == Op::move) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto move = dynamic_cast<R_Inst *>(inst2);
            if (li->rt == move->rs) {
                li->rt = move->rd;
                assemblies.erase(assem2);
                flag = true;
            }
        }
    }

    return flag;
}

// Load
bool MIPS::allMergeMove_R_rs() {
    bool flag = false;
    for (auto assem1 = assemblies.begin(); assem1 != assemblies.end() - 1; ++assem1) {
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::move && inst2) {
            auto move = dynamic_cast<R_Inst *>(inst1);

            if (auto cal = dynamic_cast<R_Inst *>(inst2)) {
                if (cal->rs == move->rd) {
                    cal->rs = move->rs;
                    assem1 = assemblies.erase(assem1);
                    flag = true;
                }
            }
        }
    }

    return flag;
}

bool MIPS::allMergeMove_R_rt() {
    bool flag = false;
    for (auto assem1 = assemblies.begin(); assem1 != assemblies.end() - 1; ++assem1) {
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::move && inst2) {
            auto move = dynamic_cast<R_Inst *>(inst1);

            if (auto cal = dynamic_cast<R_Inst *>(inst2)) {
                if (cal->rt == move->rd) {
                    cal->rt = move->rs;
                    assem1 = assemblies.erase(assem1);
                    flag = true;
                }
            }
        }
    }

    return flag;
}

bool MIPS::allMergeR_Move() {
    bool flag = false;
    for (auto assem2 = assemblies.begin() + 1; assem2 != assemblies.end(); ++assem2) {
        auto assem1 = assem2 - 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        // inst2 && inst2->op == Op::move && ...
        if (inst1) {
            auto move = dynamic_cast<R_Inst *>(inst2);

            if (auto cal = dynamic_cast<R_Inst *>(inst1)) {
                if (cal->op != Op::move && cal->rd == move->rs) {
                    cal->rd = move->rd;
                    assem2 = assemblies.erase(assem2);
                    flag = true;
                }
            }
        }
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
