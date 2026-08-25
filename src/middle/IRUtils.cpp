#include "middle/IRUtils.h"

#include <cstdint>

namespace IR {

const Temp *asTemp(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const Temp *>(element.get());
}

const ConstVal *asConstant(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const ConstVal *>(element.get());
}

const Label *asLabel(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const Label *>(element.get());
}

const Var *asVar(const std::unique_ptr<Element> &element) {
    return dynamic_cast<const Var *>(element.get());
}

int nextTempId(const Function &function) {
    int next = 0;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                if (temp && temp->id >= next) {
                    next = temp->id + 1;
                }
            }
        }
    }
    return next;
}

std::optional<int> definedTemp(const Inst &inst) {
    if (!inst.definesTemp()) {
        return std::nullopt;
    }
    const auto *temp = dynamic_cast<const Temp *>(inst.res.get());
    if (!temp || temp->id < 0) {
        return std::nullopt;
    }
    return temp->id;
}

TempSet usedTemps(const Inst &inst) {
    TempSet result;
    for (const auto &operand: inst.operands()) {
        if (operand.role == OperandRole::Definition || !operand.value) {
            continue;
        }
        const auto *temp = dynamic_cast<const Temp *>(operand.value);
        if (temp && temp->id >= 0) {
            result.insert(temp->id);
        }
    }
    return result;
}

TempDefinitions collectTempDefinitions(const Function &function) {
    TempDefinitions result;
    const auto &blocks = function.getBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        for (size_t instruction = 0;
             instruction < blocks[block]->instructions.size();
             ++instruction) {
            if (auto definition = definedTemp(blocks[block]->instructions[instruction])) {
                result[*definition] = {block, instruction};
            }
        }
    }
    return result;
}

TempConstants collectImmediateConstants(const Function &function) {
    TempConstants result;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
            const auto *constant = inst.op == Op::LoadImd
                                           ? dynamic_cast<const ConstVal *>(inst.arg1.get())
                                           : nullptr;
            if (destination && constant) {
                result[destination->id] = constant->value;
            }
        }
    }
    return result;
}

TempConstants collectPropagatedConstants(const Function &function) {
    TempConstants result;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
                if (!destination || result.count(destination->id) != 0) {
                    continue;
                }
                if (inst.op == Op::LoadImd) {
                    const auto *value = dynamic_cast<const ConstVal *>(inst.arg1.get());
                    if (value) {
                        result[destination->id] = value->value;
                        changed = true;
                    }
                } else if (inst.op == Op::NewMove) {
                    const auto *source = dynamic_cast<const Temp *>(inst.arg1.get());
                    auto value = source ? result.find(source->id) : result.end();
                    if (value != result.end()) {
                        result[destination->id] = value->second;
                        changed = true;
                    }
                }
            }
        }
    }
    return result;
}

std::optional<int> constantValue(
        const Element *element,
        const TempConstants &knownConstants) {
    if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
        return constant->value;
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    auto value = temp ? knownConstants.find(temp->id) : knownConstants.end();
    return value == knownConstants.end() ? std::nullopt
                                         : std::optional<int>(value->second);
}

bool sameTemp(const Element *element, int id) {
    const auto *temp = dynamic_cast<const Temp *>(element);
    return temp && temp->id == id;
}

bool isCommutative(Op op) {
    return op == Op::Add || op == Op::Mul || op == Op::And || op == Op::Or
           || op == Op::Xor || op == Op::Eql || op == Op::Neq;
}

size_t instructionCost(const Function &function) {
    size_t cost = 0;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            cost += !inst.isScopeMarker();
        }
    }
    return cost;
}

int normalizeForType(int value, Type type) {
    return type == Type::Char ? value & 0xFF : value;
}

int wrappingAdd(int lhs, int rhs) {
    return static_cast<int32_t>(static_cast<uint32_t>(lhs)
                                + static_cast<uint32_t>(rhs));
}

int wrappingMultiply(int lhs, int rhs) {
    return static_cast<int32_t>(static_cast<uint32_t>(lhs)
                                * static_cast<uint32_t>(rhs));
}

} // namespace IR
