#ifndef COMPILER_MIDDLE_OPTIMIZE_H
#define COMPILER_MIDDLE_OPTIMIZE_H

#include "middle/IR.h"

namespace IR {

bool propagateGlobalConstantsAndCopies(Function &function);
bool forwardScalarLoads(Function &function);
bool eliminateCommonSubexpressions(Function &function);
bool eliminateDeadScalarStores(Function &function);
bool hoistLoopInvariantCode(Function &function);
bool reduceInductionVariableStrength(Function &function);
bool eliminateTailRecursion(Function &function);
bool hoistReadOnlyGlobalLoads(Function &function);
bool promoteMemoryToRegisters(Function &function);
bool lowerPhiNodes(Function &function);
bool simplifyPhiNodes(Function &function);
bool globalValueNumberingCodeMotion(Function &function);
bool sparseConditionalConstantPropagation(Function &function);
bool normalizeBasicBlocks(Function &function);
bool simplifyControlFlow(Function &function);
bool canonicalizeLoops(Function &function);
bool eliminatePartialRedundancy(Function &function);
bool optimizeMemoryValues(Function &function);
bool orderBasicBlocks(Function &function);
bool inlineFunctions(Module &module);
bool eliminateUnreachableFunctions(Module &module);

} // namespace IR

#endif
