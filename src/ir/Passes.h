#ifndef COMPILER_IR_PASSES_H
#define COMPILER_IR_PASSES_H

#include "ir/IR.h"

namespace IR {

void runScalarMem2Reg(Module &module);

} // namespace IR

#endif
