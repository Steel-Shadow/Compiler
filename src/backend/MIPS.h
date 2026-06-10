//
// Created by Steel_Shadow on 2023/10/31.
//

#ifndef COMPILER_MIPS_H
#define COMPILER_MIPS_H

#include "middle/IR.h"

#include <fstream>

// init register at beginning
namespace MIPS {
struct I_imm_Inst;
struct R_Inst;

extern int curDepth;

constexpr int wordSize = 4;
extern std::ofstream mipsFileStream;

struct Assembly {
    virtual ~Assembly() = default;

    virtual std::string toString() = 0;
};

extern std::vector<std::unique_ptr<Assembly>> assemblies;

// Label for MIPS instruction
struct Label : public Assembly {
    std::string nameAndId;

    explicit Label(std::string name_and_id);

    explicit Label(const IR::Label *label);

    std::string toString() override;
};

void irToMips(const IR::Inst &inst);

std::unique_ptr<I_imm_Inst> mergeLi_R(const I_imm_Inst &, const R_Inst &);

bool allMergeMove_R_rt();
bool allMergeLi_R();

bool allMergeLi_Move();

bool allMergeMove_R_rs();
bool allMergeR_Move();

void genMIPS(const IR::Module &module);
void reset();

void output(const std::string &str, bool newLine = true);

} // namespace MIPS
#endif
