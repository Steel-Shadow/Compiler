#include "middle/Optimize.h"

#include "errorHandler/Error.h"
#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

struct KnownConstant {
    int value{};
    Type type{Type::Int};
};

std::unordered_map<int, int> countTempDefinitions(const Function &function) {
    std::unordered_map<int, int> counts;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = definedTemp(inst)) {
                ++counts[*definition];
            }
        }
    }
    return counts;
}

bool hasSingleDefinition(const std::unordered_map<int, int> &counts, int temp) {
    auto definition = counts.find(temp);
    return definition != counts.end() && definition->second == 1;
}

bool getKnownConstant(const Element *element,
                      const std::unordered_map<int, KnownConstant> &constants,
                      KnownConstant &result) {
    if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
        result = {constant->value, constant->type};
        return true;
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    if (!temp || temp->id < 0) {
        return false;
    }
    auto known = constants.find(temp->id);
    if (known == constants.end()) {
        return false;
    }
    result = known->second;
    return true;
}

bool evaluateBinary(Op op, int lhs, int rhs, int &result) {
    switch (op) {
        case Op::Add:
            result = lhs + rhs;
            return true;
        case Op::Sub:
            result = lhs - rhs;
            return true;
        case Op::Mul:
            result = lhs * rhs;
            return true;
        case Op::Div:
            if (rhs == 0) {
                return false;
            }
            result = lhs / rhs;
            return true;
        case Op::Mod:
            if (rhs == 0) {
                return false;
            }
            result = lhs % rhs;
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

bool evaluateInstruction(const Inst &inst,
                         const std::unordered_map<int, KnownConstant> &constants,
                         KnownConstant &result) {
    const auto *destination = asTemp(inst.res);
    if (!destination || destination->id < 0) {
        return false;
    }

    KnownConstant lhs;
    KnownConstant rhs;
    const bool hasLhs = getKnownConstant(inst.arg1.get(), constants, lhs);
    const bool hasRhs = getKnownConstant(inst.arg2.get(), constants, rhs);
    int value = 0;

    switch (inst.op) {
        case Op::LoadImd:
        case Op::NewMove:
            if (!hasLhs) {
                return false;
            }
            value = lhs.value;
            break;
        case Op::Neg:
            if (!hasLhs) {
                return false;
            }
            value = -lhs.value;
            break;
        case Op::Not:
            if (!hasLhs) {
                return false;
            }
            value = !lhs.value;
            break;
        case Op::MulImd:
            if (!hasLhs || !hasRhs) {
                return false;
            }
            value = lhs.value * rhs.value;
            break;
        case Op::Mult4:
            if (!hasLhs) {
                return false;
            }
            value = lhs.value * 4;
            break;
        default:
            if (!hasLhs || !hasRhs || !evaluateBinary(inst.op, lhs.value, rhs.value, value)) {
                return false;
            }
            break;
    }

    result = {normalizeForType(value, destination->type), destination->type};
    return true;
}

bool isSameImmediate(const Inst &inst, const KnownConstant &constant) {
    const auto *value = asConstant(inst.arg1);
    const auto *destination = asTemp(inst.res);
    return inst.op == Op::LoadImd && value && destination
           && value->value == constant.value && value->type == constant.type
           && destination->type == constant.type;
}

void replaceWithImmediate(Inst &inst, const KnownConstant &constant) {
    auto result = inst.res ? inst.res->clone() : nullptr;
    inst = Inst(Op::LoadImd,
                std::move(result),
                std::make_unique<ConstVal>(constant.value, constant.type),
                nullptr);
}

bool propagateConstants(Function &function) {
    const auto definitionCounts = countTempDefinitions(function);
    std::unordered_map<int, KnownConstant> constants;
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool discovered = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                auto definition = definedTemp(inst);
                if (!definition || !hasSingleDefinition(definitionCounts, *definition)) {
                    continue;
                }
                KnownConstant value;
                if (evaluateInstruction(inst, constants, value)) {
                    auto [position, inserted] = constants.insert_or_assign(*definition, value);
                    (void) position;
                    discovered = discovered || inserted;
                }
            }
        }
        if (!discovered) {
            break;
        }
    }

    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        auto &instructions = block->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                KnownConstant condition;
                if (getKnownConstant(inst.arg1.get(), constants, condition)) {
                    const bool taken = inst.op == Op::Bif1 ? condition.value != 0 : condition.value == 0;
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
            if (definition) {
                auto known = constants.find(*definition);
                if (known != constants.end() && !isSameImmediate(inst, known->second)) {
                    replaceWithImmediate(inst, known->second);
                    changed = true;
                }
            }
            ++index;
        }
    }
    return changed;
}

bool propagateCopies(Function &function) {
    struct Copy {
        Temp destination;
        Temp source;
    };
    const auto definitionCounts = countTempDefinitions(function);
    std::vector<Copy> copies;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != Op::NewMove) {
                continue;
            }
            const auto *destination = asTemp(inst.res);
            const auto *source = asTemp(inst.arg1);
            if (!destination || !source || destination->id < 0 || source->id < 0
                || destination->type != source->type
                || !hasSingleDefinition(definitionCounts, destination->id)
                || !hasSingleDefinition(definitionCounts, source->id)) {
                continue;
            }
            copies.push_back({*destination, *source});
        }
    }

    bool changed = false;
    for (const auto &copy: copies) {
        changed = function.replaceAllUsesWith(copy.destination, copy.source) || changed;
    }
    return changed;
}

bool simplifyBooleanBranchConditions(Function &function) {
    std::unordered_map<int, const Inst *> definitions;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = definedTemp(inst)) {
                definitions[*definition] = &inst;
            }
        }
    }

    auto isZero = [&](const Element *element) {
        if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
            return constant->value == 0;
        }
        const auto *temp = dynamic_cast<const Temp *>(element);
        auto definition = temp ? definitions.find(temp->id) : definitions.end();
        if (definition == definitions.end() || definition->second->op != Op::LoadImd) {
            return false;
        }
        const auto *constant = asConstant(definition->second->arg1);
        return constant && constant->value == 0;
    };

    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        for (auto &branch: block->instructions) {
            if (branch.op != Op::Bif0 && branch.op != Op::Bif1) {
                continue;
            }
            const auto *condition = asTemp(branch.arg1);
            auto definition = condition ? definitions.find(condition->id) : definitions.end();
            if (definition == definitions.end()) {
                continue;
            }

            const Inst &comparison = *definition->second;
            const Element *source = nullptr;
            bool inverted = false;
            if (comparison.op == Op::Not) {
                source = comparison.arg1.get();
                inverted = true;
            } else if (comparison.op == Op::Eql || comparison.op == Op::Neq) {
                if (isZero(comparison.arg1.get())) {
                    source = comparison.arg2.get();
                } else if (isZero(comparison.arg2.get())) {
                    source = comparison.arg1.get();
                }
                inverted = comparison.op == Op::Eql;
            }
            if (!source) {
                continue;
            }

            branch.arg1 = source->clone();
            if (inverted) {
                branch.op = branch.op == Op::Bif0 ? Op::Bif1 : Op::Bif0;
            }
            changed = true;
        }
    }
    return changed;
}

bool simplifyRemainderTests(Function &function) {
    const auto definitionCounts = countTempDefinitions(function);
    std::unordered_map<int, KnownConstant> constants;
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool discovered = false;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                auto definition = definedTemp(inst);
                if (!definition || !hasSingleDefinition(definitionCounts, *definition)) {
                    continue;
                }
                KnownConstant value;
                if (evaluateInstruction(inst, constants, value)) {
                    auto [position, inserted] = constants.insert_or_assign(*definition, value);
                    (void) position;
                    discovered = discovered || inserted;
                }
            }
        }
        if (!discovered) {
            break;
        }
    }

    auto useDef = function.buildUseDefChains();
    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        for (auto &inst: block->instructions) {
            if (inst.op != Op::Mod) {
                continue;
            }
            const auto *result = asTemp(inst.res);
            KnownConstant divisor;
            if (!result || !getKnownConstant(inst.arg2.get(), constants, divisor)
                || (divisor.value != 2 && divisor.value != -2)) {
                continue;
            }
            auto chain = useDef.find(result->valueKey());
            if (chain == useDef.end() || chain->second.uses.size() != 1) {
                continue;
            }
            const auto &use = chain->second.uses.front();
            if (!use.user || (use.user->op != Op::Eql && use.user->op != Op::Neq)) {
                continue;
            }
            const Element *other = nullptr;
            if (use.user->arg1 && use.user->arg1->isSameValue(*result)) {
                other = use.user->arg2.get();
            } else if (use.user->arg2 && use.user->arg2->isSameValue(*result)) {
                other = use.user->arg1.get();
            }
            KnownConstant comparison;
            if (!other || !getKnownConstant(other, constants, comparison) || comparison.value != 0) {
                continue;
            }
            inst.op = Op::And;
            inst.arg2 = std::make_unique<ConstVal>(1, Type::Int);
            changed = true;
        }
    }
    return changed;
}

bool isGVNCandidate(Op op) {
    switch (op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd:
        case Op::Neg:
        case Op::Mult4:
        case Op::Not:
            return true;
        default:
            return false;
    }
}

struct AvailableScalarValue {
    int temp;
    Type type;

    bool operator==(const AvailableScalarValue &other) const {
        return temp == other.temp && type == other.type;
    }
};

using ScalarState = std::unordered_map<Var, AvailableScalarValue>;

ScalarState meetScalarStates(const std::vector<size_t> &predecessors,
                             const std::vector<ScalarState> &outStates) {
    if (predecessors.empty()) {
        return {};
    }
    ScalarState result = outStates[predecessors.front()];
    for (size_t index = 1; index < predecessors.size(); ++index) {
        const auto &other = outStates[predecessors[index]];
        for (auto value = result.begin(); value != result.end();) {
            auto candidate = other.find(value->first);
            if (candidate == other.end() || !(candidate->second == value->second)) {
                value = result.erase(value);
            } else {
                ++value;
            }
        }
    }
    return result;
}

void transferScalarState(const Inst &inst, const VarSet &candidates, ScalarState &state) {
    const auto *var = asVar(inst.arg1);
    if (!var || candidates.find(*var) == candidates.end()) {
        return;
    }
    if (inst.op == Op::Store) {
        const auto *value = asTemp(inst.res);
        if (value && value->id >= 0) {
            state[*var] = {value->id, value->type};
        } else {
            state.erase(*var);
        }
    } else if (inst.op == Op::Load) {
        const auto *value = asTemp(inst.res);
        if (value && value->id >= 0 && state.find(*var) == state.end()) {
            state[*var] = {value->id, value->type};
        }
    }
}

std::string expressionKey(const Inst &inst,
                          const std::unordered_map<int, std::string> &canonicalConstants) {
    const auto *destination = asTemp(inst.res);
    if (!destination || !isGVNCandidate(inst.op)) {
        return {};
    }

    auto operandKey = [&](const std::unique_ptr<Element> &operand) {
        const auto *temp = asTemp(operand);
        if (temp) {
            auto constant = canonicalConstants.find(temp->id);
            if (constant != canonicalConstants.end()) {
                return constant->second;
            }
        }
        return operand ? operand->valueKey() : std::string("_");
    };
    std::string lhs = operandKey(inst.arg1);
    std::string rhs = operandKey(inst.arg2);
    if (isCommutative(inst.op) && rhs < lhs) {
        std::swap(lhs, rhs);
    }
    return std::to_string(static_cast<int>(inst.op)) + ":"
           + std::to_string(static_cast<int>(destination->type)) + ":" + lhs + ":" + rhs;
}

bool isSafeToHoist(Op op) {
    switch (op) {
        case Op::LoadImd:
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd:
        case Op::Neg:
        case Op::Mult4:
        case Op::Not:
            return true;
        default:
            return false;
    }
}

using LoopBlocks = std::unordered_set<size_t>;

std::unordered_map<size_t, LoopBlocks> findNaturalLoops(const ControlFlowGraph &cfg) {
    std::unordered_map<size_t, LoopBlocks> loops;
    for (size_t tail = 0; tail < cfg.successors.size(); ++tail) {
        for (size_t header: cfg.successors[tail]) {
            if (!cfg.dominates(header, tail)) {
                continue;
            }
            auto &loop = loops[header];
            loop.insert(header);
            if (!loop.insert(tail).second && tail == header) {
                continue;
            }
            std::vector<size_t> work{tail};
            while (!work.empty()) {
                const size_t block = work.back();
                work.pop_back();
                for (size_t predecessor: cfg.predecessors[block]) {
                    if (loop.insert(predecessor).second && predecessor != header) {
                        work.push_back(predecessor);
                    }
                }
            }
        }
    }
    return loops;
}

bool operandsAreLoopInvariant(const Inst &inst,
                              const LoopBlocks &loop,
                              const std::unordered_map<int, size_t> &definitionBlock,
                              const TempSet &invariantTemps) {
    for (const auto &operand: inst.operands()) {
        if (operand.role == OperandRole::Definition || !operand.value) {
            continue;
        }
        const auto *temp = dynamic_cast<const Temp *>(operand.value);
        if (!temp) {
            continue;
        }
        if (temp->id < 0) {
            return false;
        }
        auto definition = definitionBlock.find(temp->id);
        if (definition == definitionBlock.end()) {
            return false;
        }
        if (loop.find(definition->second) != loop.end()
            && invariantTemps.find(temp->id) == invariantTemps.end()) {
            return false;
        }
    }
    return true;
}

int nextAvailableTempId(const Function &function) {
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

bool isParameterVariable(const Function &function, const Var &var) {
    return std::any_of(function.getParams().begin(), function.getParams().end(), [&](const ParamInfo &param) {
        return var == Var(param.name, 1, false, param.dims, param.type, !param.dims.empty());
    });
}

bool isSupportedPromotionUse(const Inst &inst, const OperandRef &operand) {
    if (operand.slot != 1) {
        return false;
    }
    if (inst.op == Op::Alloca) {
        const auto *size = asConstant(inst.arg2);
        return size && size->value == 1;
    }
    if (inst.op == Op::Load) {
        return !inst.arg2;
    }
    if (inst.op == Op::Store) {
        return !inst.arg2 && asTemp(inst.res);
    }
    return false;
}

bool hasLoadWithoutDefinition(const Function &function,
                              const ControlFlowGraph &cfg,
                              const Var &var,
                              bool parameter) {
    const size_t blockCount = function.getBasicBlocks().size();
    std::vector<bool> definedIn(blockCount, true);
    std::vector<bool> definedOut(blockCount, true);

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t block = 0; block < blockCount; ++block) {
            bool nextIn = parameter;
            if (!cfg.predecessors[block].empty()) {
                nextIn = true;
                for (size_t predecessor: cfg.predecessors[block]) {
                    nextIn = nextIn && definedOut[predecessor];
                }
            }

            bool nextOut = nextIn;
            for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
                const auto *stored = asVar(inst.arg1);
                if (inst.op == Op::Store && stored && *stored == var && !inst.arg2) {
                    nextOut = true;
                }
            }
            if (definedIn[block] != nextIn || definedOut[block] != nextOut) {
                definedIn[block] = nextIn;
                definedOut[block] = nextOut;
                changed = true;
            }
        }
    }

    for (size_t block = 0; block < blockCount; ++block) {
        bool defined = definedIn[block];
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            const auto *accessed = asVar(inst.arg1);
            if (!accessed || !(*accessed == var) || inst.arg2) {
                continue;
            }
            if (inst.op == Op::Load && !defined) {
                return true;
            }
            if (inst.op == Op::Store) {
                defined = true;
            }
        }
    }
    return false;
}

Temp resolveTempReplacement(const Temp &value,
                            const std::unordered_map<int, Temp> &replacements) {
    Temp result(value);
    std::unordered_set<int> visited;
    while (result.id >= 0 && visited.insert(result.id).second) {
        auto replacement = replacements.find(result.id);
        if (replacement == replacements.end()) {
            break;
        }
        result = replacement->second;
    }
    return result;
}

struct PhiCopy {
    Temp destination;
    std::unique_ptr<Element> source;

    PhiCopy(const Temp &destination, std::unique_ptr<Element> source) :
        destination(destination),
        source(std::move(source)) {}
    PhiCopy(PhiCopy &&) noexcept = default;
    PhiCopy &operator=(PhiCopy &&) noexcept = default;
};

std::vector<Inst> scheduleParallelCopies(std::vector<PhiCopy> copies, int &nextTemp) {
    copies.erase(
            std::remove_if(copies.begin(), copies.end(), [](const PhiCopy &copy) {
                const auto *source = dynamic_cast<const Temp *>(copy.source.get());
                return source && source->id == copy.destination.id;
            }),
            copies.end());

    std::vector<Inst> result;
    while (!copies.empty()) {
        auto safe = std::find_if(copies.begin(), copies.end(), [&](const PhiCopy &copy) {
            return std::none_of(copies.begin(), copies.end(), [&](const PhiCopy &other) {
                const auto *source = dynamic_cast<const Temp *>(other.source.get());
                return source && source->id == copy.destination.id;
            });
        });

        if (safe == copies.end()) {
            const Temp saved(nextTemp++, copies.front().destination.type);
            result.emplace_back(Op::NewMove,
                                std::make_unique<Temp>(saved),
                                std::make_unique<Temp>(copies.front().destination),
                                nullptr);
            for (auto &copy: copies) {
                const auto *source = dynamic_cast<const Temp *>(copy.source.get());
                if (source && source->id == copies.front().destination.id) {
                    copy.source = std::make_unique<Temp>(saved);
                }
            }
            continue;
        }

        if (const auto *source = dynamic_cast<const Temp *>(safe->source.get())) {
            result.emplace_back(Op::NewMove,
                                std::make_unique<Temp>(safe->destination),
                                std::make_unique<Temp>(*source),
                                nullptr);
        } else if (const auto *constant = dynamic_cast<const ConstVal *>(safe->source.get())) {
            result.emplace_back(Op::LoadImd,
                                std::make_unique<Temp>(safe->destination),
                                std::make_unique<ConstVal>(*constant),
                                nullptr);
        } else {
            Error::raise("Unsupported phi incoming value");
        }
        copies.erase(safe);
    }
    return result;
}

} // namespace

bool promoteMemoryToRegisters(Function &function) {
    if (function.getBasicBlocks().empty()) {
        return false;
    }

    const auto cfg = buildControlFlowGraph(function);
    const auto liveness = analyzeVariableLiveness(function, cfg);
    VarSet candidates = liveness.candidates;
    VarSet unsupported;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                const auto *var = dynamic_cast<const Var *>(operand.value);
                if (var && candidates.find(*var) != candidates.end()
                    && !isSupportedPromotionUse(inst, operand)) {
                    unsupported.insert(*var);
                }
            }
        }
    }
    for (const auto &var: unsupported) {
        candidates.erase(var);
    }

    std::vector<Var> orderedCandidates(candidates.begin(), candidates.end());
    std::sort(orderedCandidates.begin(), orderedCandidates.end());
    for (auto candidate = orderedCandidates.begin(); candidate != orderedCandidates.end();) {
        const bool parameter = isParameterVariable(function, *candidate);
        if (hasLoadWithoutDefinition(function, cfg, *candidate, parameter)) {
            candidates.erase(*candidate);
            candidate = orderedCandidates.erase(candidate);
        } else {
            ++candidate;
        }
    }
    if (orderedCandidates.empty()) {
        return false;
    }

    std::unordered_map<Var, std::unordered_set<size_t>> definitionBlocks;
    for (const auto &var: orderedCandidates) {
        if (isParameterVariable(function, var)) {
            definitionBlocks[var].insert(0);
        }
    }
    for (size_t block = 0; block < function.getBasicBlocks().size(); ++block) {
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            const auto *var = asVar(inst.arg1);
            if (inst.op == Op::Store && var && candidates.find(*var) != candidates.end()) {
                definitionBlocks[*var].insert(block);
            }
        }
    }

    std::unordered_map<size_t, std::vector<Var>> blockPhis;
    for (const auto &var: orderedCandidates) {
        std::queue<size_t> work;
        std::unordered_set<size_t> queued = definitionBlocks[var];
        std::unordered_set<size_t> placed;
        std::vector<size_t> definitions(queued.begin(), queued.end());
        std::sort(definitions.begin(), definitions.end());
        for (size_t block: definitions) {
            work.push(block);
        }
        while (!work.empty()) {
            const size_t block = work.front();
            work.pop();
            for (size_t frontier: cfg.dominanceFrontier[block]) {
                if (liveness.liveIn[frontier].find(var) == liveness.liveIn[frontier].end()
                    || !placed.insert(frontier).second) {
                    continue;
                }
                blockPhis[frontier].push_back(var);
                if (queued.insert(frontier).second) {
                    work.push(frontier);
                }
            }
        }
    }

    int nextTemp = nextAvailableTempId(function);
    auto &blocks = function.getMutableBasicBlocks();
    for (auto &[block, vars]: blockPhis) {
        std::sort(vars.begin(), vars.end());
        std::vector<Inst> phis;
        phis.reserve(vars.size());
        for (const auto &var: vars) {
            phis.emplace_back(Op::Phi,
                              std::make_unique<Temp>(nextTemp++, ptrToValue(var.type)),
                              std::make_unique<Var>(var),
                              nullptr);
        }
        blocks[block]->instructions.insert(
                blocks[block]->instructions.begin(),
                std::make_move_iterator(phis.begin()),
                std::make_move_iterator(phis.end()));
    }

    size_t phiCount = 0;
    while (phiCount < blocks.front()->instructions.size()
           && blocks.front()->instructions[phiCount].op == Op::Phi) {
        ++phiCount;
    }
    std::vector<Inst> parameters;
    for (const auto &param: function.getParams()) {
        Var var(param.name, 1, false, param.dims, param.type, !param.dims.empty());
        if (candidates.find(var) == candidates.end()) {
            continue;
        }
        parameters.emplace_back(Op::Parameter,
                                std::make_unique<Temp>(nextTemp++, ptrToValue(var.type)),
                                std::make_unique<Var>(var),
                                nullptr);
    }
    blocks.front()->instructions.insert(
            blocks.front()->instructions.begin() + static_cast<long>(phiCount),
            std::make_move_iterator(parameters.begin()),
            std::make_move_iterator(parameters.end()));

    std::unordered_map<Var, std::vector<Temp>> valueStacks;
    std::unordered_map<int, Temp> replacements;
    bool failed = false;
    std::function<void(size_t)> rename = [&](size_t blockIndex) {
        std::unordered_map<Var, size_t> pushed;
        auto pushValue = [&](const Var &var, const Temp &value) {
            valueStacks[var].push_back(resolveTempReplacement(value, replacements));
            ++pushed[var];
        };

        auto &instructions = blocks[blockIndex]->instructions;
        for (auto &inst: instructions) {
            if (inst.op != Op::Phi) {
                break;
            }
            const auto *var = asVar(inst.arg1);
            const auto *result = asTemp(inst.res);
            if (var && result && candidates.find(*var) != candidates.end()) {
                pushValue(*var, *result);
            }
        }

        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            const auto *var = asVar(inst.arg1);
            if (inst.op == Op::Phi) {
                ++index;
                continue;
            }
            if (inst.op == Op::Parameter && var && candidates.find(*var) != candidates.end()) {
                const auto *result = asTemp(inst.res);
                if (result) {
                    pushValue(*var, *result);
                }
                ++index;
                continue;
            }
            if (!var || candidates.find(*var) == candidates.end()) {
                ++index;
                continue;
            }
            if (inst.op == Op::Alloca) {
                instructions.erase(instructions.begin() + static_cast<long>(index));
                continue;
            }
            if (inst.op == Op::Store) {
                const auto *stored = asTemp(inst.res);
                if (!stored) {
                    failed = true;
                    ++index;
                    continue;
                }
                pushValue(*var, *stored);
                instructions.erase(instructions.begin() + static_cast<long>(index));
                continue;
            }
            if (inst.op == Op::Load) {
                const auto *loaded = asTemp(inst.res);
                auto values = valueStacks.find(*var);
                if (!loaded || values == valueStacks.end() || values->second.empty()) {
                    failed = true;
                    ++index;
                    continue;
                }
                replacements.insert_or_assign(
                        loaded->id,
                        resolveTempReplacement(values->second.back(), replacements));
                instructions.erase(instructions.begin() + static_cast<long>(index));
                continue;
            }
            ++index;
        }

        for (size_t successor: cfg.successors[blockIndex]) {
            for (auto &phi: blocks[successor]->instructions) {
                if (phi.op != Op::Phi) {
                    break;
                }
                const auto *var = asVar(phi.arg1);
                auto values = var ? valueStacks.find(*var) : valueStacks.end();
                if (!var || values == valueStacks.end() || values->second.empty()) {
                    failed = true;
                    continue;
                }
                const Temp value = resolveTempReplacement(values->second.back(), replacements);
                phi.addPhiIncoming(
                        blocks[blockIndex]->label.nameAndId,
                        std::make_unique<Temp>(value));
            }
        }

        for (size_t child: cfg.dominatorTree[blockIndex]) {
            rename(child);
        }
        for (const auto &[var, count]: pushed) {
            auto &values = valueStacks[var];
            values.erase(values.end() - static_cast<long>(count), values.end());
        }
    };
    rename(0);

    if (failed) {
        Error::raise("mem2reg encountered an undefined promoted value");
        return false;
    }

    for (auto &block: blocks) {
        for (auto &inst: block->instructions) {
            const auto operands = inst.operands();
            for (const auto &operand: operands) {
                if (operand.role == OperandRole::Definition) {
                    continue;
                }
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                if (!temp || temp->id < 0) {
                    continue;
                }
                const Temp replacement = resolveTempReplacement(*temp, replacements);
                if (replacement.id != temp->id) {
                    inst.setOperand(operand.slot, std::make_unique<Temp>(replacement));
                }
            }
        }
    }
    return true;
}

bool lowerPhiNodes(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }

    std::unordered_map<std::string, size_t> labelToBlock;
    std::unordered_set<std::string> labels;
    for (size_t block = 0; block < blocks.size(); ++block) {
        labelToBlock.emplace(blocks[block]->label.nameAndId, block);
        labels.insert(blocks[block]->label.nameAndId);
    }

    std::map<std::pair<size_t, size_t>, std::vector<PhiCopy>> edgeCopies;
    bool changed = false;
    for (size_t successor = 0; successor < blocks.size(); ++successor) {
        auto &instructions = blocks[successor]->instructions;
        for (const auto &phi: instructions) {
            if (phi.op != Op::Phi) {
                continue;
            }
            const auto *destination = asTemp(phi.res);
            if (!destination) {
                Error::raise("Phi without a temporary result");
                continue;
            }
            for (const auto &incoming: phi.phiIncoming) {
                auto predecessor = labelToBlock.find(incoming.predecessor);
                if (predecessor == labelToBlock.end() || !incoming.value) {
                    Error::raise("Phi incoming edge does not exist");
                    continue;
                }
                edgeCopies[{predecessor->second, successor}].emplace_back(
                        *destination, incoming.value->clone());
            }
            changed = true;
        }
        instructions.erase(
                std::remove_if(instructions.begin(), instructions.end(), [](const Inst &inst) {
                    return inst.op == Op::Phi;
                }),
                instructions.end());
    }
    if (!changed) {
        return false;
    }

    int nextTemp = nextAvailableTempId(function);
    int splitId = 0;
    auto uniqueSplitLabel = [&]() {
        std::string name;
        do {
            name = "__phi_edge_" + function.getName() + "_"
                   + std::to_string(splitId++);
        } while (!labels.insert(name).second);
        return name;
    };

    for (auto &[edge, copies]: edgeCopies) {
        auto sequence = scheduleParallelCopies(std::move(copies), nextTemp);
        if (sequence.empty()) {
            continue;
        }
        const size_t predecessor = edge.first;
        const size_t successor = edge.second;
        if (cfg.successors[predecessor].size() <= 1) {
            auto &instructions = blocks[predecessor]->instructions;
            auto insertion = std::find_if(instructions.begin(), instructions.end(), [](const Inst &inst) {
                return inst.isTerminator();
            });
            instructions.insert(insertion,
                                std::make_move_iterator(sequence.begin()),
                                std::make_move_iterator(sequence.end()));
            continue;
        }

        auto split = std::make_unique<BasicBlock>("__phi_edge");
        split->label.nameAndId = uniqueSplitLabel();
        split->instructions.insert(split->instructions.end(),
                                   std::make_move_iterator(sequence.begin()),
                                   std::make_move_iterator(sequence.end()));
        split->instructions.emplace_back(
                Op::Br, nullptr, blocks[successor]->label.clone(), nullptr);

        bool retargeted = false;
        for (auto &inst: blocks[predecessor]->instructions) {
            std::unique_ptr<Element> *target = nullptr;
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                target = &inst.arg2;
            } else if (inst.op == Op::Br) {
                target = &inst.arg1;
            }
            const auto *label = target ? dynamic_cast<const Label *>(target->get()) : nullptr;
            if (label && label->nameAndId == blocks[successor]->label.nameAndId) {
                *target = std::make_unique<Label>(split->label);
                retargeted = true;
            }
        }
        if (!retargeted) {
            blocks[predecessor]->instructions.emplace_back(
                    Op::Br, nullptr, std::make_unique<Label>(split->label), nullptr);
        }
        blocks.emplace_back(std::move(split));
    }
    return true;
}

bool simplifyPhiNodes(Function &function) {
    bool changed = false;
    bool simplified = true;
    while (simplified) {
        simplified = false;
        const auto cfg = buildControlFlowGraph(function);
        auto &blocks = function.getMutableBasicBlocks();
        for (size_t block = 0; block < blocks.size(); ++block) {
            std::unordered_set<std::string> predecessors;
            for (size_t predecessor: cfg.predecessors[block]) {
                predecessors.insert(blocks[predecessor]->label.nameAndId);
            }

            auto &instructions = blocks[block]->instructions;
            for (size_t index = 0; index < instructions.size();) {
                auto &phi = instructions[index];
                if (phi.op != Op::Phi) {
                    ++index;
                    continue;
                }
                const size_t oldSize = phi.phiIncoming.size();
                std::unordered_set<std::string> seen;
                phi.phiIncoming.erase(
                        std::remove_if(
                                phi.phiIncoming.begin(),
                                phi.phiIncoming.end(),
                                [&](const PhiIncoming &incoming) {
                                    return predecessors.find(incoming.predecessor) == predecessors.end()
                                           || !seen.insert(incoming.predecessor).second;
                                }),
                        phi.phiIncoming.end());
                if (phi.phiIncoming.size() != oldSize) {
                    changed = true;
                }
                if (phi.phiIncoming.empty()) {
                    Error::raise("Phi has no reachable incoming edge");
                    ++index;
                    continue;
                }

                const Element *replacement = phi.phiIncoming.front().value.get();
                bool sameValue = replacement != nullptr;
                for (size_t incoming = 1; incoming < phi.phiIncoming.size(); ++incoming) {
                    sameValue = sameValue && phi.phiIncoming[incoming].value
                                && phi.phiIncoming[incoming].value->isSameValue(*replacement);
                }
                const auto *destination = asTemp(phi.res);
                if (!sameValue || !destination) {
                    ++index;
                    continue;
                }

                auto replacementCopy = replacement->clone();
                function.replaceAllUsesWith(*destination, *replacementCopy);
                instructions.erase(instructions.begin() + static_cast<long>(index));
                changed = true;
                simplified = true;
            }
        }
    }
    return changed;
}

bool propagateGlobalConstantsAndCopies(Function &function) {
    const bool constantsChanged = propagateConstants(function);
    const bool remainderChanged = simplifyRemainderTests(function);
    const bool copiesChanged = propagateCopies(function);
    const bool branchesChanged = simplifyBooleanBranchConditions(function);
    return constantsChanged || remainderChanged || copiesChanged || branchesChanged;
}

bool forwardScalarLoads(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto liveness = analyzeVariableLiveness(function, cfg);
    const size_t blockCount = function.getBasicBlocks().size();
    std::vector<ScalarState> inStates(blockCount);
    std::vector<ScalarState> outStates(blockCount);

    struct UniqueStore {
        size_t block;
        AvailableScalarValue value;
        int count;
    };
    std::unordered_map<Var, UniqueStore> uniqueStores;
    for (size_t block = 0; block < blockCount; ++block) {
        for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
            const auto *var = asVar(inst.arg1);
            const auto *value = asTemp(inst.res);
            if (inst.op != Op::Store || !var || !value || value->id < 0
                || liveness.candidates.find(*var) == liveness.candidates.end()) {
                continue;
            }
            auto store = uniqueStores.find(*var);
            if (store == uniqueStores.end()) {
                uniqueStores.emplace(*var, UniqueStore{block, {value->id, value->type}, 1});
            } else {
                ++store->second.count;
            }
        }
    }

    std::unordered_map<int, AvailableScalarValue> replacements;
    auto resolveReplacement = [&](AvailableScalarValue value) {
        std::unordered_set<int> visited;
        while (visited.insert(value.temp).second) {
            auto replacement = replacements.find(value.temp);
            if (replacement == replacements.end()) {
                break;
            }
            value = replacement->second;
        }
        return value;
    };

    bool dataflowChanged = true;
    for (int iteration = 0; iteration < 64 && dataflowChanged; ++iteration) {
        dataflowChanged = false;
        for (size_t block = 0; block < blockCount; ++block) {
            ScalarState nextIn = block == 0 ? ScalarState{}
                                            : meetScalarStates(cfg.predecessors[block], outStates);
            ScalarState nextOut = nextIn;
            for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
                transferScalarState(inst, liveness.candidates, nextOut);
            }
            if (nextIn != inStates[block] || nextOut != outStates[block]) {
                inStates[block] = std::move(nextIn);
                outStates[block] = std::move(nextOut);
                dataflowChanged = true;
            }
        }
    }

    bool changed = false;
    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block < blockCount; ++block) {
        ScalarState state = inStates[block];
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            const auto *var = asVar(inst.arg1);
            const auto *destination = asTemp(inst.res);
            if (inst.op == Op::Load && var && destination && destination->id >= 0
                && liveness.candidates.find(*var) != liveness.candidates.end()) {
                std::optional<AvailableScalarValue> replacement;
                auto available = state.find(*var);
                if (available != state.end() && available->second.type == destination->type
                    && available->second.temp != destination->id) {
                    replacement = resolveReplacement(available->second);
                }
                auto uniqueStore = uniqueStores.find(*var);
                if (!replacement && uniqueStore != uniqueStores.end()
                    && uniqueStore->second.count == 1
                    && uniqueStore->second.block != block
                    && cfg.dominates(uniqueStore->second.block, block)
                    && uniqueStore->second.value.type == destination->type) {
                    replacement = resolveReplacement(uniqueStore->second.value);
                }
                if (replacement && replacement->temp != destination->id) {
                    Temp replacementTemp(replacement->temp, replacement->type);
                    function.replaceAllUsesWith(*destination, replacementTemp);
                    replacements[destination->id] = *replacement;
                    state[*var] = *replacement;
                    instructions.erase(instructions.begin() + static_cast<long>(index));
                    changed = true;
                    continue;
                }
            }
            transferScalarState(inst, liveness.candidates, state);
            ++index;
        }
    }
    return changed;
}

bool eliminateCommonSubexpressions(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    if (cfg.successors.empty()) {
        return false;
    }

    struct NumberedValue {
        int id;
        Type type;
    };
    using AvailableExpressions = std::unordered_map<std::string, NumberedValue>;
    bool changed = false;
    const auto definitionCounts = countTempDefinitions(function);
    std::unordered_map<int, std::string> canonicalConstants;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = definedTemp(inst)) {
                const auto *constant = asConstant(inst.arg1);
                if (hasSingleDefinition(definitionCounts, *definition)
                    && inst.op == Op::LoadImd && constant) {
                    canonicalConstants[*definition] = "constant:"
                                                      + std::to_string(static_cast<int>(constant->type)) + ":"
                                                      + std::to_string(constant->value);
                }
            }
        }
    }

    std::function<void(size_t, AvailableExpressions)> visit =
            [&](size_t blockIndex, AvailableExpressions available) {
                auto &instructions = function.getMutableBasicBlocks()[blockIndex]->instructions;
                for (size_t index = 0; index < instructions.size();) {
                    auto &inst = instructions[index];
                    const auto key = expressionKey(inst, canonicalConstants);
                    const auto *destination = asTemp(inst.res);
                    bool singleDefinitionOperands = true;
                    for (int operand: usedTemps(inst)) {
                        if (!hasSingleDefinition(definitionCounts, operand)) {
                            singleDefinitionOperands = false;
                            break;
                        }
                    }
                    if (key.empty() || !destination
                        || !hasSingleDefinition(definitionCounts, destination->id)
                        || !singleDefinitionOperands) {
                        ++index;
                        continue;
                    }

                    auto existing = available.find(key);
                    if (existing != available.end()) {
                        Temp replacement(existing->second.id, existing->second.type);
                        function.replaceAllUsesWith(*destination, replacement);
                        instructions.erase(instructions.begin() + static_cast<long>(index));
                        changed = true;
                        continue;
                    }
                    available.emplace(key, NumberedValue{destination->id, destination->type});
                    ++index;
                }
                for (size_t child: cfg.dominatorTree[blockIndex]) {
                    visit(child, available);
                }
            };

    for (size_t block = 0; block < cfg.successors.size(); ++block) {
        if (block == 0 || cfg.immediateDominator[block] == ControlFlowGraph::NoBlock) {
            visit(block, {});
        }
    }
    return changed;
}

bool eliminateDeadScalarStores(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto liveness = analyzeVariableLiveness(function, cfg);
    bool changed = false;

    auto &blocks = function.getMutableBasicBlocks();
    for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
        VarSet live = liveness.liveOut[blockIndex];
        auto &instructions = blocks[blockIndex]->instructions;
        for (size_t reverse = instructions.size(); reverse-- > 0;) {
            auto &inst = instructions[reverse];
            const auto *var = asVar(inst.arg1);
            if (inst.op == Op::Store && var && liveness.candidates.find(*var) != liveness.candidates.end()) {
                if (live.find(*var) == live.end()) {
                    instructions.erase(instructions.begin() + static_cast<long>(reverse));
                    changed = true;
                    continue;
                }
                live.erase(*var);
            } else if (inst.op == Op::Load && var
                       && liveness.candidates.find(*var) != liveness.candidates.end()) {
                live.insert(*var);
            }
        }
    }
    return changed;
}

bool hoistLoopInvariantCode(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto loops = findNaturalLoops(cfg);
    if (loops.empty()) {
        return false;
    }

    const auto definitionCounts = countTempDefinitions(function);
    std::unordered_map<int, size_t> definitionBlock;
    std::unordered_set<int> knownNonZeroConstants;
    const auto &constBlocks = function.getBasicBlocks();
    for (size_t block = 0; block < constBlocks.size(); ++block) {
        for (const auto &inst: constBlocks[block]->instructions) {
            if (auto definition = definedTemp(inst);
                definition && hasSingleDefinition(definitionCounts, *definition)) {
                definitionBlock[*definition] = block;
                const auto *constant = inst.op == Op::LoadImd
                                               ? dynamic_cast<const ConstVal *>(inst.arg1.get())
                                               : nullptr;
                if (constant && constant->value != 0) {
                    knownNonZeroConstants.insert(*definition);
                }
            }
        }
    }

    for (const auto &[header, loop]: loops) {
        std::vector<size_t> outsidePredecessors;
        for (size_t predecessor: cfg.predecessors[header]) {
            if (loop.find(predecessor) == loop.end()) {
                outsidePredecessors.push_back(predecessor);
            }
        }
        if (outsidePredecessors.size() != 1) {
            continue;
        }
        const size_t preheaderIndex = outsidePredecessors.front();
        if (cfg.successors[preheaderIndex].size() != 1
            || cfg.successors[preheaderIndex].front() != header) {
            continue;
        }

        TempSet invariantTemps;
        std::vector<Inst> hoisted;
        bool movedInRound = true;
        auto &blocks = function.getMutableBasicBlocks();
        while (movedInRound) {
            movedInRound = false;
            for (size_t block = 0; block < blocks.size(); ++block) {
                if (loop.find(block) == loop.end()) {
                    continue;
                }
                auto &instructions = blocks[block]->instructions;
                for (size_t index = 0; index < instructions.size();) {
                    auto &inst = instructions[index];
                    auto definition = definedTemp(inst);
                    const auto *divisor = dynamic_cast<const Temp *>(inst.arg2.get());
                    const auto *immediateDivisor = dynamic_cast<const ConstVal *>(
                            inst.arg2.get());
                    const bool safeDivision = (inst.op == Op::Div || inst.op == Op::Mod)
                                              && ((divisor
                                                   && knownNonZeroConstants.count(divisor->id) != 0)
                                                  || (immediateDivisor
                                                      && immediateDivisor->value != 0));
                    if (!definition || !hasSingleDefinition(definitionCounts, *definition)
                        || (!isSafeToHoist(inst.op) && !safeDivision)
                        || !operandsAreLoopInvariant(inst, loop, definitionBlock, invariantTemps)) {
                        ++index;
                        continue;
                    }
                    invariantTemps.insert(*definition);
                    hoisted.emplace_back(std::move(inst));
                    instructions.erase(instructions.begin() + static_cast<long>(index));
                    movedInRound = true;
                }
            }
        }

        if (hoisted.empty()) {
            continue;
        }
        auto &preheader = blocks[preheaderIndex]->instructions;
        auto insertion = std::find_if(preheader.begin(), preheader.end(), [](const Inst &inst) {
            return inst.isTerminator();
        });
        preheader.insert(insertion,
                         std::make_move_iterator(hoisted.begin()),
                         std::make_move_iterator(hoisted.end()));
        return true;
    }
    return false;
}

bool eliminateTailRecursion(Function &function) {
    if (function.getBasicBlocks().empty() || function.getParams().empty()) {
        return false;
    }
    if (std::any_of(function.getParams().begin(), function.getParams().end(), [](const ParamInfo &param) {
            return !param.dims.empty();
        })) {
        return false;
    }

    struct TailCall {
        size_t frameStart{};
        size_t call{};
        size_t frameEnd{};
        size_t moveResult{};
        size_t ret{};
        std::vector<size_t> pushes;
    };

    auto findTailCall = [&](const BasicBlock &block) -> std::optional<TailCall> {
        const auto &instructions = block.instructions;
        for (size_t call = 0; call < instructions.size(); ++call) {
            const auto &callInst = instructions[call];
            const auto *callee = dynamic_cast<const Label *>(callInst.arg1.get());
            if (callInst.op != Op::Call || !callee || callee->nameAndId != function.getName()) {
                continue;
            }

            size_t nestedFrames = 0;
            size_t frameStart = call;
            for (size_t reverse = call; reverse-- > 0;) {
                if (instructions[reverse].op == Op::OutStack) {
                    ++nestedFrames;
                } else if (instructions[reverse].op == Op::InStack) {
                    if (nestedFrames == 0) {
                        frameStart = reverse;
                        break;
                    }
                    --nestedFrames;
                }
            }
            if (frameStart == call) {
                continue;
            }

            std::vector<size_t> pushes;
            bool unsupportedArgument = false;
            for (size_t index = frameStart + 1; index < call; ++index) {
                if (instructions[index].op == Op::PushAddressParam) {
                    unsupportedArgument = true;
                    break;
                }
                if (instructions[index].op == Op::PushParam) {
                    pushes.push_back(index);
                }
            }
            if (unsupportedArgument || pushes.size() != function.getParams().size()) {
                continue;
            }

            size_t after = call + 1;
            if (after >= instructions.size() || instructions[after].op != Op::OutStack) {
                continue;
            }
            const size_t frameEnd = after++;
            size_t moveResult = instructions.size();
            if (function.getReturnType() != Type::Void) {
                if (after >= instructions.size() || instructions[after].op != Op::NewMove) {
                    continue;
                }
                const auto *returnRegister = dynamic_cast<const Temp *>(instructions[after].arg1.get());
                if (!returnRegister || returnRegister->id != -2) {
                    continue;
                }
                moveResult = after++;
            }
            if (after >= instructions.size() || instructions[after].op != Op::Ret) {
                continue;
            }
            if (moveResult != instructions.size()) {
                const auto *moved = dynamic_cast<const Temp *>(instructions[moveResult].res.get());
                const auto *returned = dynamic_cast<const Temp *>(instructions[after].arg1.get());
                if (!moved || !returned || moved->id != returned->id) {
                    continue;
                }
            } else if (instructions[after].arg1) {
                continue;
            }
            return TailCall{frameStart, call, frameEnd, moveResult, after, std::move(pushes)};
        }
        return std::nullopt;
    };

    bool hasTailCall = false;
    for (const auto &block: function.getBasicBlocks()) {
        if (findTailCall(*block)) {
            hasTailCall = true;
            break;
        }
    }
    if (!hasTailCall) {
        return false;
    }

    auto &blocks = function.getMutableBasicBlocks();
    auto loopHeader = std::make_unique<BasicBlock>(function.getName() + "_tail");
    const Label loopLabel = loopHeader->label;
    loopHeader->instructions = std::move(blocks.front()->instructions);
    blocks.front()->instructions.emplace_back(
            Op::Br, nullptr, loopLabel.clone(), nullptr);
    blocks.insert(blocks.begin() + 1, std::move(loopHeader));

    bool changed = false;
    for (size_t blockIndex = 1; blockIndex < blocks.size(); ++blockIndex) {
        auto tailCall = findTailCall(*blocks[blockIndex]);
        if (!tailCall) {
            continue;
        }

        auto &instructions = blocks[blockIndex]->instructions;
        std::vector<std::unique_ptr<Element>> arguments;
        arguments.reserve(tailCall->pushes.size());
        for (auto push = tailCall->pushes.rbegin(); push != tailCall->pushes.rend(); ++push) {
            arguments.push_back(instructions[*push].arg1->clone());
        }

        std::unordered_set<size_t> removed(tailCall->pushes.begin(), tailCall->pushes.end());
        removed.insert(tailCall->frameStart);
        removed.insert(tailCall->call);
        removed.insert(tailCall->frameEnd);
        removed.insert(tailCall->ret);
        if (tailCall->moveResult != instructions.size()) {
            removed.insert(tailCall->moveResult);
        }

        std::vector<Inst> rewritten;
        rewritten.reserve(instructions.size() + function.getParams().size());
        for (size_t index = 0; index < instructions.size(); ++index) {
            if (index == tailCall->call) {
                for (size_t param = 0; param < function.getParams().size(); ++param) {
                    const auto &formal = function.getParams()[param];
                    rewritten.emplace_back(
                            Op::Store,
                            std::move(arguments[param]),
                            std::make_unique<Var>(formal.name, 1, false, formal.dims, formal.type, false),
                            nullptr);
                }
                rewritten.emplace_back(Op::Br, nullptr, loopLabel.clone(), nullptr);
            }
            if (removed.find(index) == removed.end()) {
                rewritten.emplace_back(std::move(instructions[index]));
            }
        }
        instructions = std::move(rewritten);
        changed = true;
    }
    return changed;
}

bool hoistReadOnlyGlobalLoads(Function &function) {
    for (const auto &block: function.getBasicBlocks()) {
        if (std::any_of(block->instructions.begin(), block->instructions.end(), [](const Inst &inst) {
                return inst.op == Op::Call;
            })) {
            return false;
        }
    }

    std::unordered_map<Var, std::vector<Temp>> loads;
    std::unordered_set<Var> modified;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            const auto *var = asVar(inst.arg1);
            if (!var || var->depth != 0 || !var->dims.empty() || inst.arg2) {
                continue;
            }
            if (inst.op == Op::Load) {
                const auto *result = asTemp(inst.res);
                if (result && result->id >= 0) {
                    loads[*var].push_back(*result);
                }
            } else if (inst.op == Op::Store || inst.op == Op::StoreDynamic
                       || inst.op == Op::MemZero
                       || inst.op == Op::GetString) {
                modified.insert(*var);
            }
        }
    }

    bool changed = false;
    auto &blocks = function.getMutableBasicBlocks();
    for (const auto &[var, results]: loads) {
        const Var &currentVar = var;
        if (results.empty() || modified.find(var) != modified.end()) {
            continue;
        }
        const Temp canonical = results.front();
        const bool alreadyInEntry = std::any_of(
                blocks.front()->instructions.begin(),
                blocks.front()->instructions.end(),
                [&](const Inst &inst) {
                    const auto *loaded = asVar(inst.arg1);
                    const auto *result = asTemp(inst.res);
                    return inst.op == Op::Load && loaded && *loaded == currentVar && !inst.arg2
                           && result && result->id == canonical.id;
                });
        if (results.size() == 1 && alreadyInEntry) {
            continue;
        }
        for (size_t index = 1; index < results.size(); ++index) {
            function.replaceAllUsesWith(results[index], canonical);
        }

        std::optional<Inst> hoisted;
        for (auto &block: blocks) {
            auto &instructions = block->instructions;
            for (size_t index = 0; index < instructions.size();) {
                const auto *loaded = asVar(instructions[index].arg1);
                if (instructions[index].op != Op::Load || !loaded || !(*loaded == var)
                    || instructions[index].arg2) {
                    ++index;
                    continue;
                }
                if (!hoisted) {
                    hoisted.emplace(std::move(instructions[index]));
                }
                instructions.erase(instructions.begin() + static_cast<long>(index));
            }
        }
        if (!hoisted) {
            continue;
        }
        auto &entry = blocks.front()->instructions;
        entry.insert(entry.begin(), std::move(*hoisted));
        changed = true;
    }
    return changed;
}

} // namespace IR
