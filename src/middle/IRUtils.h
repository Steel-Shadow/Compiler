#ifndef COMPILER_MIDDLE_IR_UTILS_H
#define COMPILER_MIDDLE_IR_UTILS_H

#include "middle/IR.h"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace IR {

struct TempDefinition {
    size_t block{};
    size_t instruction{};
};

using TempDefinitions = std::unordered_map<int, TempDefinition>;
using TempConstants = std::unordered_map<int, int>;
using TempSet = std::unordered_set<int>;

const Temp *asTemp(const std::unique_ptr<Element> &element);
const ConstVal *asConstant(const std::unique_ptr<Element> &element);
const Label *asLabel(const std::unique_ptr<Element> &element);
const Var *asVar(const std::unique_ptr<Element> &element);

int nextTempId(const Function &function);
std::optional<int> definedTemp(const Inst &inst);
TempSet usedTemps(const Inst &inst);
TempDefinitions collectTempDefinitions(const Function &function);
TempConstants collectImmediateConstants(const Function &function);
TempConstants collectPropagatedConstants(const Function &function);

std::optional<int> constantValue(
        const Element *element,
        const TempConstants &knownConstants);
bool sameTemp(const Element *element, int id);
bool isCommutative(Op op);

size_t instructionCost(const Function &function);
int normalizeForType(int value, Type type);
int wrappingAdd(int lhs, int rhs);
int wrappingMultiply(int lhs, int rhs);

} // namespace IR

#endif
