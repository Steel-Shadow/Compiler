//
// Created by Steel_Shadow on 2023/11/15.
//

#ifndef REGISTER_H
#define REGISTER_H

#include "middle/IR.h"

#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

namespace MIPS {
// @formatter:off
enum class Register {
    zero,

    // used by assembler, I can use too, but must confirm that it don't conflict with pseudo MIPS
    at,

    // return value of function
    v0,
    v1,

    // parameter of function
    a0,
    a1,
    a2,
    a3,

    // $t0-$t6 and unused $s0-$s7 are graph-colored values.
    // $t7-$t9 are reserved for spill/scratch materialization.
    t0,
    t1,
    t2,
    t3,
    t4,
    t5,
    t6,
    t7,

    s0,
    s1,
    s2,
    s3,
    s4,
    s5,
    s6,
    s7,
    t8,
    t9,

    k0,
    k1,
    gp,
    sp,

    // Reserved as an address scratch register by the instruction selector.
    fp,

    ra,
    none,
}; // @formatter:on


constexpr int MAX_TEMP_REGS = 7;
constexpr int MAX_VAR_REGS = 8;
constexpr int CALL_FRAME_RESERVED_WORDS = 1 + MAX_TEMP_REGS + MAX_VAR_REGS;

extern std::map<int, Register> tempToRegs;
extern std::map<IR::Var, Register> varToRegs;
extern std::map<IR::Var, Register> allocatedVarRegs;

void prepareRegisterAllocation(const IR::Function &function);
void configureRegisterArgumentConvention(const IR::Module &module);
void reserveSpillSlots();
void beginInstruction(const IR::Inst &inst);
void endInstruction();

void emitFunctionPrologue(const IR::Function &function, bool isMain);
void emitFunctionEpilogue();

std::vector<Register> liveTempRegistersAfter(const IR::Inst &inst);
std::vector<Register> usedVariableRegisters();

bool usesRegisterArguments(const IR::Function &function);
Register argumentRegister(size_t index);

int tempSaveOffset(Register reg);
int variableSaveOffset(Register reg);

Register newReg(const IR::Temp *temp);

Register getReg(const IR::Temp *temp);
Register acquireScratchRegister();

std::optional<int> knownConstant(const IR::Element *element);

void checkTempReg(const IR::Temp *temp, Register reg);
bool isRematerialized(const IR::Temp *temp);

void clearRegs();

std::string regToString(Register reg);

} // namespace MIPS

#endif
