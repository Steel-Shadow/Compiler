#include "middle/Optimize.h"

#include "middle/Analysis.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

enum class LatticeKind {
    Undefined,
    Constant,
    Overdefined,
};

struct LatticeValue {
    LatticeKind kind{LatticeKind::Undefined};
    int value{};
    Type type{Type::Int};

    bool operator==(const LatticeValue &other) const {
        return kind == other.kind
               && (kind != LatticeKind::Constant
                   || (value == other.value && type == other.type));
    }
};

struct Edge {
    size_t from;
    size_t to;

    bool operator==(const Edge &other) const {
        return from == other.from && to == other.to;
    }
};

struct EdgeHash {
    size_t operator()(const Edge &edge) const {
        return edge.from * 0x9E3779B97F4A7C15ULL + edge.to;
    }
};

int normalize(int value, Type type) {
    return type == Type::Char ? value & 0xFF : value;
}

LatticeValue constant(int value, Type type) {
    return {LatticeKind::Constant, normalize(value, type), type};
}

LatticeValue valueOf(const Element *element,
                     const std::unordered_map<int, LatticeValue> &values) {
    if (const auto *immediate = dynamic_cast<const ConstVal *>(element)) {
        return constant(immediate->value, immediate->type);
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    if (!temp || temp->id < 0) {
        return {LatticeKind::Overdefined, 0, temp ? temp->type : Type::Int};
    }
    auto value = values.find(temp->id);
    return value == values.end() ? LatticeValue{} : value->second;
}

LatticeValue meet(LatticeValue lhs, const LatticeValue &rhs) {
    if (lhs.kind == LatticeKind::Undefined) {
        return rhs;
    }
    if (rhs.kind == LatticeKind::Undefined) {
        return lhs;
    }
    if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined) {
        return {LatticeKind::Overdefined, 0, lhs.type};
    }
    if (lhs.value == rhs.value) {
        return lhs;
    }
    return {LatticeKind::Overdefined, 0, lhs.type};
}

bool evaluateBinary(Op op, int lhs, int rhs, int &result) {
    const int64_t wideLhs = lhs;
    const int64_t wideRhs = rhs;
    switch (op) {
        case Op::Add:
            result = static_cast<int>(wideLhs + wideRhs);
            return true;
        case Op::Sub:
            result = static_cast<int>(wideLhs - wideRhs);
            return true;
        case Op::Mul:
        case Op::MulImd:
            result = static_cast<int>(wideLhs * wideRhs);
            return true;
        case Op::Div:
            if (rhs == 0) {
                return false;
            }
            result = static_cast<int>(wideLhs / wideRhs);
            return true;
        case Op::Mod:
            if (rhs == 0) {
                return false;
            }
            result = static_cast<int>(wideLhs % wideRhs);
            return true;
        case Op::And:
            result = lhs & rhs;
            return true;
        case Op::Or:
            result = lhs | rhs;
            return true;
        case Op::Leq:
            result = lhs <= rhs;
            return true;
        case Op::Lss:
            result = lhs < rhs;
            return true;
        case Op::Geq:
            result = lhs >= rhs;
            return true;
        case Op::Gre:
            result = lhs > rhs;
            return true;
        case Op::Eql:
            result = lhs == rhs;
            return true;
        case Op::Neq:
            result = lhs != rhs;
            return true;
        default:
            return false;
    }
}

LatticeValue evaluateInstruction(
        const Inst &inst,
        Type resultType,
        const std::unordered_map<int, LatticeValue> &values,
        const std::unordered_set<Edge, EdgeHash> &executableEdges,
        size_t block,
        const std::unordered_map<std::string, size_t> &labels) {
    if (inst.op == Op::Phi) {
        LatticeValue result;
        for (const auto &incoming: inst.phiIncoming) {
            auto predecessor = labels.find(incoming.predecessor);
            if (predecessor == labels.end()
                || executableEdges.find({predecessor->second, block}) == executableEdges.end()) {
                continue;
            }
            result = meet(result, valueOf(incoming.value.get(), values));
        }
        if (result.kind == LatticeKind::Constant) {
            result.type = resultType;
            result.value = normalize(result.value, resultType);
        }
        return result;
    }

    const LatticeValue lhs = valueOf(inst.arg1.get(), values);
    const LatticeValue rhs = valueOf(inst.arg2.get(), values);
    auto unary = [&](auto operation) {
        if (lhs.kind == LatticeKind::Undefined) {
            return LatticeValue{};
        }
        if (lhs.kind == LatticeKind::Overdefined) {
            return LatticeValue{LatticeKind::Overdefined, 0, resultType};
        }
        return constant(operation(lhs.value), resultType);
    };

    switch (inst.op) {
        case Op::LoadImd:
        case Op::NewMove:
            if (lhs.kind == LatticeKind::Constant) {
                return constant(lhs.value, resultType);
            }
            return {lhs.kind, 0, resultType};
        case Op::Neg:
            return unary([](int value) { return static_cast<int>(-static_cast<int64_t>(value)); });
        case Op::Not:
            return unary([](int value) { return !value; });
        case Op::Mult4:
            return unary([](int value) { return static_cast<int>(static_cast<int64_t>(value) * 4); });
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd: {
            if (lhs.kind == LatticeKind::Undefined || rhs.kind == LatticeKind::Undefined) {
                return {};
            }
            if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined) {
                return {LatticeKind::Overdefined, 0, resultType};
            }
            int result = 0;
            if (!evaluateBinary(inst.op, lhs.value, rhs.value, result)) {
                return {LatticeKind::Overdefined, 0, resultType};
            }
            return constant(result, resultType);
        }
        default:
            return {LatticeKind::Overdefined, 0, resultType};
    }
}

} // namespace

bool sparseConditionalConstantPropagation(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }

    std::unordered_map<std::string, size_t> labels;
    for (size_t block = 0; block < blocks.size(); ++block) {
        labels.emplace(blocks[block]->label.nameAndId, block);
    }

    std::unordered_map<int, LatticeValue> values;
    std::vector<bool> reachable(blocks.size(), false);
    std::unordered_set<Edge, EdgeHash> executableEdges;
    reachable[0] = true;

    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t block = 0; block < blocks.size(); ++block) {
            if (!reachable[block]) {
                continue;
            }
            for (const auto &inst: blocks[block]->instructions) {
                auto definition = definedTemp(inst);
                const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
                if (!definition || !destination) {
                    continue;
                }
                const LatticeValue evaluated = evaluateInstruction(
                        inst, destination->type, values, executableEdges, block, labels);
                const LatticeValue merged = meet(values[*definition], evaluated);
                if (!(merged == values[*definition])) {
                    values[*definition] = merged;
                    progress = true;
                }
            }

            auto markEdge = [&](size_t successor) {
                if (executableEdges.insert({block, successor}).second) {
                    progress = true;
                }
                if (!reachable[successor]) {
                    reachable[successor] = true;
                    progress = true;
                }
            };

            bool terminated = false;
            for (const auto &inst: blocks[block]->instructions) {
                if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                    const auto *target = dynamic_cast<const Label *>(inst.arg2.get());
                    auto targetBlock = target ? labels.find(target->nameAndId) : labels.end();
                    const LatticeValue condition = valueOf(inst.arg1.get(), values);
                    if (condition.kind == LatticeKind::Undefined) {
                        terminated = true;
                        break;
                    }
                    const bool mayTake = condition.kind == LatticeKind::Overdefined
                                         || (inst.op == Op::Bif1 ? condition.value != 0
                                                                : condition.value == 0);
                    const bool mayFallThrough = condition.kind == LatticeKind::Overdefined
                                                || !mayTake;
                    if (mayTake && targetBlock != labels.end()) {
                        markEdge(targetBlock->second);
                    }
                    if (!mayFallThrough) {
                        terminated = true;
                        break;
                    }
                    continue;
                }
                if (inst.op == Op::Br) {
                    const auto *target = dynamic_cast<const Label *>(inst.arg1.get());
                    auto targetBlock = target ? labels.find(target->nameAndId) : labels.end();
                    if (targetBlock != labels.end()) {
                        markEdge(targetBlock->second);
                    }
                    terminated = true;
                    break;
                }
                if (inst.op == Op::Ret || inst.op == Op::RetMain) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated && block + 1 < blocks.size()) {
                markEdge(block + 1);
            }
        }
    }

    bool changed = false;
    for (size_t block = 0; block < blocks.size(); ++block) {
        if (!reachable[block]) {
            continue;
        }
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                const LatticeValue condition = valueOf(inst.arg1.get(), values);
                if (condition.kind == LatticeKind::Constant) {
                    const bool taken = inst.op == Op::Bif1 ? condition.value != 0
                                                          : condition.value == 0;
                    if (taken) {
                        inst = Inst(Op::Br, nullptr, inst.arg2 ? inst.arg2->clone() : nullptr, nullptr);
                        ++index;
                    } else {
                        instructions.erase(instructions.begin() + static_cast<long>(index));
                    }
                    changed = true;
                    continue;
                }
            }

            auto definition = definedTemp(inst);
            const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
            auto known = definition ? values.find(*definition) : values.end();
            if (destination && known != values.end()
                && known->second.kind == LatticeKind::Constant) {
                const auto *old = dynamic_cast<const ConstVal *>(inst.arg1.get());
                const bool alreadyConstant = inst.op == Op::LoadImd && old
                                             && old->value == known->second.value
                                             && old->type == destination->type;
                if (!alreadyConstant) {
                    auto result = inst.res->clone();
                    inst = Inst(Op::LoadImd,
                                std::move(result),
                                std::make_unique<ConstVal>(known->second.value, destination->type),
                                nullptr);
                    changed = true;
                }
            }
            ++index;
        }
    }

    if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
        BasicBlocks kept;
        kept.reserve(blocks.size());
        for (size_t block = 0; block < blocks.size(); ++block) {
            if (reachable[block]) {
                kept.push_back(std::move(blocks[block]));
            }
        }
        blocks = std::move(kept);
        changed = true;
    }
    if (changed) {
        simplifyPhiNodes(function);
    }
    return changed;
}

} // namespace IR
