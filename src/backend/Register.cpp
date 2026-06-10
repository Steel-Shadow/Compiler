//
// Created by Steel_Shadow on 2023/11/15.
//

#include "Register.h"
#include "Instruction.h"
#include "Memory.h"
#include "MIPS.h"

using namespace MIPS;

// use template to dynamically generate
template<std::size_t... Indices>
constexpr auto genCleanRegs(std::index_sequence<Indices...>, bool tempElseVar) {
    return std::queue<Register>{{static_cast<Register>(Indices + static_cast<size_t>(tempElseVar ? Register::t0 : Register::s0))...}};
}

template<std::size_t N>
constexpr auto cleanRegQueue(bool tempElseVar) {
    return genCleanRegs<>(std::make_index_sequence<N>{}, tempElseVar);
}

std::map<int, Register> MIPS::tempToRegs;
std::queue<Register> MIPS::freeTempRegs = cleanRegQueue<MAX_TEMP_REGS>(true);

std::map<IR::Var, Register> MIPS::varToRegs;
std::queue<Register> MIPS::freeVarRegs = cleanRegQueue<MAX_VAR_REGS>(false);

Register MIPS::newReg(const IR::Temp *temp) {
    if (temp->id < 0) {
        return static_cast<Register>(-temp->id);
    } else if (freeTempRegs.empty()) {
        return Register::fp;
    } else {
        Register r = freeTempRegs.front();
        freeTempRegs.pop();
        tempToRegs[temp->id] = r;
        return r;
    }
}

Register MIPS::getReg(const IR::Temp *temp) {
    if (temp->id < 0) {
        return static_cast<Register>(-temp->id);
    } else {
        auto tempToReg = tempToRegs.find(temp->id);
        if (tempToReg == tempToRegs.end()) {
            // tempToReg not found, temp has been stored in memory
            assemblies.push_back(std::make_unique<I_imm_Inst>(
                    Op::lw,
                    Register::fp,
                    Register::sp,
                    -StackMemory::varToOffset[IR::Var(temp->toString(), -1)]));
            return Register::fp;
        } else {
            Register t = tempToReg->second;
            freeTempRegs.push(tempToReg->second);
            tempToRegs.erase(temp->id);
            return t;
        }
    }
}

std::string MIPS::regToString(Register reg) {
    switch (reg) {
        case Register::zero:
            return "$zero";
        case Register::at:
            return "$at";
        case Register::v0:
            return "$v0";
        case Register::v1:
            return "$v1";
        case Register::a0:
            return "$a0";
        case Register::a1:
            return "$a1";
        case Register::a2:
            return "$a2";
        case Register::a3:
            return "$a3";
        case Register::t0:
            return "$t0";
        case Register::t1:
            return "$t1";
        case Register::t2:
            return "$t2";
        case Register::t3:
            return "$t3";
        case Register::t4:
            return "$t4";
        case Register::t5:
            return "$t5";
        case Register::t6:
            return "$t6";
        case Register::t7:
            return "$t7";
        case Register::s0:
            return "$s0";
        case Register::s1:
            return "$s1";
        case Register::s2:
            return "$s2";
        case Register::s3:
            return "$s3";
        case Register::s4:
            return "$s4";
        case Register::s5:
            return "$s5";
        case Register::s6:
            return "$s6";
        case Register::s7:
            return "$s7";
        case Register::t8:
            return "$t8";
        case Register::t9:
            return "$t9";
        case Register::k0:
            return "$k0";
        case Register::k1:
            return "$k1";
        case Register::gp:
            return "$gp";
        case Register::sp:
            return "$sp";
        case Register::fp:
            return "$fp";
        case Register::ra:
            return "$ra";
        case Register::none:
            return "";
        default:
            return "unknown";
    }
}

void MIPS::clearRegs() {
    tempToRegs.clear();
    freeTempRegs = cleanRegQueue<MAX_TEMP_REGS>(true);

    varToRegs.clear();
    freeVarRegs = cleanRegQueue<MAX_VAR_REGS>(false);
}

void MIPS::checkTempReg(const IR::Temp *temp, Register reg) {
    if (reg == Register::fp) {
        // if freeTempRegs is empty (reg==$t8),
        // we should store temp on stack
        auto var = IR::Var(temp->toString(), -1);
        StackMemory::curOffset += wordSize;
        StackMemory::varToOffset[var] = StackMemory::curOffset;
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sw,
                                                          Register::fp,
                                                          Register::sp,
                                                          -StackMemory::curOffset));
    }
}
