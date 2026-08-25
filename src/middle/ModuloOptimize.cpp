#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IR {
namespace {

struct NonnegativeFact {
    bool initialized{};
    bool nonnegative{};
    std::int64_t upperExclusive{};

    bool operator==(const NonnegativeFact &other) const {
        return initialized == other.initialized
               && nonnegative == other.nonnegative
               && upperExclusive == other.upperExclusive;
    }
};

NonnegativeFact factFor(
        const Element *element,
        const std::unordered_map<int, NonnegativeFact> &facts) {
    if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
        if (constant->value < 0) {
            return {true, false, 0};
        }
        return {true, true, static_cast<std::int64_t>(constant->value) + 1};
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    auto found = temp ? facts.find(temp->id) : facts.end();
    return found == facts.end() ? NonnegativeFact{true, false, 0}
                                : found->second;
}

NonnegativeFact mergeFacts(
        const NonnegativeFact &lhs,
        const NonnegativeFact &rhs) {
    if (!lhs.initialized) {
        return rhs;
    }
    if (!rhs.initialized) {
        return lhs;
    }
    if (!lhs.nonnegative || !rhs.nonnegative) {
        return {true, false, 0};
    }
    if (lhs.upperExclusive == 0 || rhs.upperExclusive == 0) {
        return {true, true, 0};
    }
    return {true, true, std::max(lhs.upperExclusive, rhs.upperExclusive)};
}

NonnegativeFact evaluateNonnegative(
        const Inst &inst,
        const std::unordered_map<int, NonnegativeFact> &facts,
        const std::unordered_map<int, int> &knownConstants) {
    const auto lhs = factFor(inst.arg1.get(), facts);
    const auto rhs = factFor(inst.arg2.get(), facts);
    switch (inst.op) {
        case Op::LoadImd:
        case Op::NewMove:
            return lhs;
        case Op::Phi: {
            NonnegativeFact result;
            for (const auto &incoming: inst.phiIncoming) {
                result = mergeFacts(result, factFor(incoming.value.get(), facts));
            }
            return result;
        }
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::Not:
            return {true, true, 2};
        case Op::And: {
            const auto leftConstant = constantValue(inst.arg1.get(), knownConstants);
            const auto rightConstant = constantValue(inst.arg2.get(), knownConstants);
            const auto mask = leftConstant && *leftConstant >= 0
                                      ? leftConstant
                              : rightConstant && *rightConstant >= 0
                                      ? rightConstant
                                      : std::nullopt;
            return mask ? NonnegativeFact{
                                  true, true,
                                  static_cast<std::int64_t>(*mask) + 1}
                        : NonnegativeFact{true, false, 0};
        }
        case Op::Add:
            if (lhs.initialized && rhs.initialized
                && lhs.nonnegative && rhs.nonnegative
                && lhs.upperExclusive > 0 && rhs.upperExclusive > 0) {
                const std::int64_t maximum = lhs.upperExclusive - 1
                                             + rhs.upperExclusive - 1;
                if (maximum <= std::numeric_limits<int>::max()) {
                    return {true, true, maximum + 1};
                }
            }
            return {true, false, 0};
        case Op::Mul:
        case Op::MulImd:
            if (lhs.initialized && rhs.initialized
                && lhs.nonnegative && rhs.nonnegative
                && lhs.upperExclusive > 0 && rhs.upperExclusive > 0) {
                const std::int64_t maximum = (lhs.upperExclusive - 1)
                                             * (rhs.upperExclusive - 1);
                if (maximum <= std::numeric_limits<int>::max()) {
                    return {true, true, maximum + 1};
                }
            }
            return {true, false, 0};
        case Op::Div: {
            const auto divisor = constantValue(inst.arg2.get(), knownConstants);
            if (lhs.initialized && lhs.nonnegative && divisor && *divisor > 0) {
                return {true, true,
                        lhs.upperExclusive > 0
                                ? (lhs.upperExclusive - 1) / *divisor + 1
                                : 0};
            }
            return {true, false, 0};
        }
        case Op::Mod: {
            const auto modulus = constantValue(inst.arg2.get(), knownConstants);
            if (lhs.initialized && lhs.nonnegative && modulus && *modulus > 0) {
                const std::int64_t bound = lhs.upperExclusive > 0
                                                   ? std::min<std::int64_t>(
                                                             lhs.upperExclusive, *modulus)
                                                   : *modulus;
                return {true, true, bound};
            }
            return {true, false, 0};
        }
        default:
            return {true, false, 0};
    }
}

std::unordered_map<int, NonnegativeFact> nonnegativeFacts(
        const Function &function,
        const std::unordered_map<int, int> &knownConstants) {
    std::unordered_map<int, NonnegativeFact> facts;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = definedTemp(inst)) {
                facts.emplace(*definition, NonnegativeFact{});
            }
        }
    }

    bool changed = true;
    for (int iteration = 0; iteration < 64 && changed; ++iteration) {
        changed = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                auto definition = definedTemp(inst);
                if (!definition) {
                    continue;
                }
                const auto evaluated = evaluateNonnegative(
                        inst, facts, knownConstants);
                const auto merged = mergeFacts(facts[*definition], evaluated);
                if (!(merged == facts[*definition])) {
                    facts[*definition] = merged;
                    changed = true;
                }
            }
        }
    }
    return facts;
}

std::unordered_map<int, int> normalizedValues(
        const Function &function,
        const std::unordered_map<int, int> &knownConstants) {
    std::unordered_map<int, int> normalized;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            const auto *result = dynamic_cast<const Temp *>(inst.res.get());
            const auto modulus = inst.op == Op::Mod
                                         ? constantValue(inst.arg2.get(), knownConstants)
                                         : std::nullopt;
            if (result && modulus && *modulus > 0) {
                normalized[result->id] = *modulus;
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                const auto *result = dynamic_cast<const Temp *>(inst.res.get());
                if (inst.op == Op::NewMove && result) {
                    const auto *source = dynamic_cast<const Temp *>(inst.arg1.get());
                    auto value = source ? normalized.find(source->id) : normalized.end();
                    if (value != normalized.end()
                        && normalized.emplace(result->id, value->second).second) {
                        changed = true;
                    }
                } else if (inst.op == Op::Phi && result && !inst.phiIncoming.empty()) {
                    std::optional<int> commonModulus;
                    bool allNormalized = true;
                    for (const auto &incoming: inst.phiIncoming) {
                        const auto *source = dynamic_cast<const Temp *>(incoming.value.get());
                        auto value = source ? normalized.find(source->id) : normalized.end();
                        if (value == normalized.end()) {
                            continue;
                        }
                        if (commonModulus && *commonModulus != value->second) {
                            allNormalized = false;
                            break;
                        }
                        commonModulus = value->second;
                    }
                    if (!commonModulus) {
                        allNormalized = false;
                    }
                    for (const auto &incoming: inst.phiIncoming) {
                        const auto *source = dynamic_cast<const Temp *>(incoming.value.get());
                        auto value = source ? normalized.find(source->id) : normalized.end();
                        if (value != normalized.end()) {
                            continue;
                        }
                        const auto constant = constantValue(
                                incoming.value.get(), knownConstants);
                        if (!constant || !commonModulus
                            || *constant <= -*commonModulus
                            || *constant >= *commonModulus) {
                            allNormalized = false;
                            break;
                        }
                    }
                    if (allNormalized && commonModulus
                        && normalized.emplace(result->id, *commonModulus).second) {
                        changed = true;
                    }
                }
            }
        }
    }
    return normalized;
}

bool normalizeLoopInitialValue(
        Function &function,
        const ControlFlowGraph &cfg,
        const std::unordered_map<size_t, std::unordered_set<size_t>> &loops,
        const std::unordered_map<int, int> &normalized) {
    auto &blocks = function.getMutableBasicBlocks();
    for (const auto &[header, loop]: loops) {
        std::vector<size_t> outsidePredecessors;
        for (size_t predecessor: cfg.predecessors[header]) {
            if (loop.count(predecessor) == 0) {
                outsidePredecessors.push_back(predecessor);
            }
        }
        if (outsidePredecessors.size() != 1
            || cfg.successors[outsidePredecessors.front()].size() != 1) {
            continue;
        }
        const size_t preheader = outsidePredecessors.front();
        for (auto &phi: blocks[header]->instructions) {
            if (phi.op != Op::Phi) {
                break;
            }
            const auto *result = dynamic_cast<const Temp *>(phi.res.get());
            if (!result || normalized.count(result->id) != 0) {
                continue;
            }
            PhiIncoming *outsideIncoming = nullptr;
            std::optional<int> modulus;
            bool loopIncomingNormalized = true;
            for (auto &incoming: phi.phiIncoming) {
                if (incoming.predecessor == blocks[preheader]->label.nameAndId) {
                    outsideIncoming = &incoming;
                    continue;
                }
                const auto *value = dynamic_cast<const Temp *>(incoming.value.get());
                auto normalizedValue = value ? normalized.find(value->id) : normalized.end();
                if (normalizedValue == normalized.end()
                    || (modulus && *modulus != normalizedValue->second)) {
                    loopIncomingNormalized = false;
                    break;
                }
                modulus = normalizedValue->second;
            }
            if (!outsideIncoming || !loopIncomingNormalized || !modulus) {
                continue;
            }
            const auto *outsideTemp = dynamic_cast<const Temp *>(outsideIncoming->value.get());
            auto outsideNormalized = outsideTemp
                                             ? normalized.find(outsideTemp->id)
                                             : normalized.end();
            if (outsideNormalized != normalized.end()
                && outsideNormalized->second == *modulus) {
                continue;
            }
            const auto outsideConstant = constantValue(
                    outsideIncoming->value.get(), collectImmediateConstants(function));
            if (outsideConstant && *outsideConstant > -*modulus
                && *outsideConstant < *modulus) {
                continue;
            }

            const Temp normalizedInitial(nextTempId(function), result->type);
            auto &instructions = blocks[preheader]->instructions;
            auto terminator = std::find_if(
                    instructions.begin(), instructions.end(),
                    [](const Inst &inst) { return inst.isTerminator(); });
            instructions.insert(
                    terminator,
                    Inst(Op::Mod,
                         normalizedInitial.clone(),
                         outsideIncoming->value->clone(),
                         std::make_unique<ConstVal>(*modulus, Type::Int)));
            outsideIncoming->value = normalizedInitial.clone();
            return true;
        }
    }
    return false;
}

bool removeIdempotentRemainder(
        Function &function,
        const std::unordered_map<int, int> &normalized,
        const std::unordered_map<int, int> &knownConstants) {
    for (auto &block: function.getMutableBasicBlocks()) {
        for (auto &inst: block->instructions) {
            if (inst.op != Op::Mod) {
                continue;
            }
            const auto *source = dynamic_cast<const Temp *>(inst.arg1.get());
            const auto modulus = constantValue(inst.arg2.get(), knownConstants);
            auto sourceModulus = source ? normalized.find(source->id) : normalized.end();
            if (!source || !modulus || sourceModulus == normalized.end()
                || sourceModulus->second != *modulus) {
                continue;
            }
            inst = Inst(Op::NewMove, std::move(inst.res), source->clone(), nullptr);
            return true;
        }
    }
    return false;
}

bool reduceBoundedModularSum(
        Function &function,
        const TempDefinitions &defs,
        const std::unordered_map<int, int> &normalized,
        const std::unordered_map<int, int> &knownConstants,
        const std::unordered_map<int, NonnegativeFact> &facts) {
    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size(); ++index) {
            const Inst &remainder = instructions[index];
            const auto *sum = remainder.op == Op::Mod
                                      ? dynamic_cast<const Temp *>(remainder.arg1.get())
                                      : nullptr;
            const auto modulus = remainder.op == Op::Mod
                                         ? constantValue(remainder.arg2.get(), knownConstants)
                                         : std::nullopt;
            auto sumDefinition = sum ? defs.find(sum->id) : defs.end();
            if (!sum || !modulus || *modulus <= 0
                || *modulus > std::numeric_limits<int>::max() / 2
                || sumDefinition == defs.end()) {
                continue;
            }
            const Inst &addition = blocks[sumDefinition->second.block]
                                           ->instructions[sumDefinition->second.instruction];
            const auto *lhs = addition.op == Op::Add
                                      ? dynamic_cast<const Temp *>(addition.arg1.get())
                                      : nullptr;
            const auto *rhs = addition.op == Op::Add
                                      ? dynamic_cast<const Temp *>(addition.arg2.get())
                                      : nullptr;
            auto lhsModulus = lhs ? normalized.find(lhs->id) : normalized.end();
            auto rhsModulus = rhs ? normalized.find(rhs->id) : normalized.end();
            if (!lhs || !rhs || lhsModulus == normalized.end()
                || rhsModulus == normalized.end()
                || lhsModulus->second != *modulus
                || rhsModulus->second != *modulus) {
                continue;
            }

            const auto lhsFact = facts.find(lhs->id);
            const auto rhsFact = facts.find(rhs->id);
            const bool nonnegativeOperands = lhsFact != facts.end()
                                             && rhsFact != facts.end()
                                             && lhsFact->second.nonnegative
                                             && rhsFact->second.nonnegative;
            if (!nonnegativeOperands) {
                continue;
            }

            int id = nextTempId(function);
            const Temp upperCondition(id++, Type::Int);
            const Temp upperCorrection(id++, Type::Int);
            std::vector<Inst> replacement;
            replacement.emplace_back(
                    Op::Geq, upperCondition.clone(), sum->clone(),
                    remainder.arg2->clone());
            replacement.emplace_back(
                    Op::Mul, upperCorrection.clone(), upperCondition.clone(),
                    remainder.arg2->clone());
            replacement.emplace_back(
                    Op::Sub, remainder.res->clone(), sum->clone(),
                    upperCorrection.clone());
            instructions.erase(instructions.begin() + static_cast<long>(index));
            instructions.insert(
                    instructions.begin() + static_cast<long>(index),
                    std::make_move_iterator(replacement.begin()),
                    std::make_move_iterator(replacement.end()));
            return true;
        }
    }
    return false;
}

} // namespace

bool optimizeModularArithmetic(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto loops = collectNaturalLoops(cfg);
    const auto knownConstants = collectImmediateConstants(function);
    const auto normalized = normalizedValues(function, knownConstants);
    if (normalizeLoopInitialValue(function, cfg, loops, normalized)) {
        return true;
    }
    if (removeIdempotentRemainder(function, normalized, knownConstants)) {
        return true;
    }
    return reduceBoundedModularSum(
            function, collectTempDefinitions(function), normalized, knownConstants,
            nonnegativeFacts(function, knownConstants));
}

} // namespace IR
