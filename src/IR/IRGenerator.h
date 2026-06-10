#ifndef COMPILER_IR_GENERATOR_H
#define COMPILER_IR_GENERATOR_H

#include "AST/CompUnit.h"
#include "IR/IR.h"

namespace IR {

Module generateModule(const CompUnit &compUnit);

} // namespace IR

#endif
