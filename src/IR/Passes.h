#ifndef COMPILER_IR_PASSES_H
#define COMPILER_IR_PASSES_H

#include "IR/IR.h"

namespace IR {

void runScalarMem2Reg(Module &module);
void runConstantPropagation(Module &module);

} // namespace IR

#endif
