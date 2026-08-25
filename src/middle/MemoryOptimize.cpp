#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

struct MemoryLocation {
    Var base;
    int offset{};

    bool operator==(const MemoryLocation &other) const {
        return base == other.base && offset == other.offset;
    }
};

struct MemoryLocationHash {
    size_t operator()(const MemoryLocation &location) const {
        return hash_value(location.base) * 0x9E3779B9U
               + static_cast<size_t>(static_cast<unsigned>(location.offset));
    }
};

struct AvailableValue {
    int temp{};
    Type type{Type::Int};

    bool operator==(const AvailableValue &other) const {
        return temp == other.temp && type == other.type;
    }
};

using MemoryState = std::unordered_map<MemoryLocation, AvailableValue, MemoryLocationHash>;

struct AffineValue {
    std::optional<int> root;
    int coefficient{};
    int constant{};
};

struct DenseArrayDefinition {
    Var base;
    size_t loopExit{};
    AffineValue offset;
    AffineValue value;
};

std::optional<AffineValue> mergeAffine(
        const AffineValue &lhs,
        const AffineValue &rhs,
        bool subtract) {
    if (lhs.root && rhs.root && lhs.root != rhs.root) {
        return std::nullopt;
    }
    const int sign = subtract ? -1 : 1;
    return AffineValue{lhs.root ? lhs.root : rhs.root,
                       wrappingAdd(lhs.coefficient,
                                   wrappingMultiply(sign, rhs.coefficient)),
                       wrappingAdd(lhs.constant,
                                   wrappingMultiply(sign, rhs.constant))};
}

std::optional<AffineValue> affineValue(
        const Element *element,
        const Function &function,
        const TempDefinitions &defs,
        std::unordered_set<int> &visiting) {
    if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
        return AffineValue{std::nullopt, 0, constant->value};
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    if (!temp || temp->id < 0) {
        return std::nullopt;
    }
    auto definition = defs.find(temp->id);
    if (definition == defs.end()) {
        return AffineValue{temp->id, 1, 0};
    }
    if (!visiting.insert(temp->id).second) {
        return AffineValue{temp->id, 1, 0};
    }

    const Inst &inst = function.getBasicBlocks()[definition->second.block]
                               ->instructions[definition->second.instruction];
    std::optional<AffineValue> result;
    if (inst.op == Op::LoadImd) {
        const auto *constant = dynamic_cast<const ConstVal *>(inst.arg1.get());
        if (constant) {
            result = AffineValue{std::nullopt, 0, constant->value};
        }
    } else if (inst.op == Op::NewMove) {
        result = affineValue(inst.arg1.get(), function, defs, visiting);
    } else if (inst.op == Op::Add || inst.op == Op::Sub) {
        auto lhs = affineValue(inst.arg1.get(), function, defs, visiting);
        auto rhs = affineValue(inst.arg2.get(), function, defs, visiting);
        if (lhs && rhs) {
            result = mergeAffine(*lhs, *rhs, inst.op == Op::Sub);
        }
    } else if (inst.op == Op::Mult4) {
        auto operand = affineValue(inst.arg1.get(), function, defs, visiting);
        if (operand) {
            operand->coefficient = wrappingMultiply(operand->coefficient, 4);
            operand->constant = wrappingMultiply(operand->constant, 4);
            result = operand;
        }
    } else if (inst.op == Op::Mul || inst.op == Op::MulImd) {
        auto lhs = affineValue(inst.arg1.get(), function, defs, visiting);
        auto rhs = affineValue(inst.arg2.get(), function, defs, visiting);
        if (lhs && rhs && (!lhs->root || !rhs->root)) {
            const AffineValue &symbolic = lhs->root ? *lhs : *rhs;
            const AffineValue &constant = lhs->root ? *rhs : *lhs;
            if (!constant.root && constant.coefficient == 0) {
                result = AffineValue{
                        symbolic.root,
                        wrappingMultiply(symbolic.coefficient, constant.constant),
                        wrappingMultiply(symbolic.constant, constant.constant)};
            }
        }
    }
    visiting.erase(temp->id);
    if (!result) {
        return AffineValue{temp->id, 1, 0};
    }
    return result;
}

std::optional<AffineValue> affineValue(
        const Element *element,
        const Function &function,
        const TempDefinitions &defs) {
    std::unordered_set<int> visiting;
    return affineValue(element, function, defs, visiting);
}

std::optional<int> constantValue(
        const Element *element,
        const Function &function,
        const TempDefinitions &defs) {
    auto value = affineValue(element, function, defs);
    if (!value || value->root || value->coefficient != 0) {
        return std::nullopt;
    }
    return value->constant;
}

std::optional<DenseArrayDefinition> denseArrayDefinition(
        const Function &function,
        const ControlFlowGraph &cfg,
        size_t header,
        const std::unordered_set<size_t> &loop,
        const TempDefinitions &defs) {
    const auto &blocks = function.getBasicBlocks();
    std::vector<size_t> outsidePredecessors;
    std::vector<size_t> latches;
    for (size_t predecessor: cfg.predecessors[header]) {
        (loop.count(predecessor) == 0 ? outsidePredecessors : latches)
                .push_back(predecessor);
    }
    if (outsidePredecessors.size() != 1 || latches.size() != 1) {
        return std::nullopt;
    }
    size_t exit = ControlFlowGraph::NoBlock;
    for (size_t block: loop) {
        for (size_t successor: cfg.successors[block]) {
            if (loop.count(successor) == 0) {
                if (block != header || (exit != ControlFlowGraph::NoBlock && exit != successor)) {
                    return std::nullopt;
                }
                exit = successor;
            }
        }
    }
    if (exit == ControlFlowGraph::NoBlock) {
        return std::nullopt;
    }

    const Temp *induction = nullptr;
    int start = 0;
    int step = 0;
    for (const auto &phi: blocks[header]->instructions) {
        if (phi.op != Op::Phi) {
            break;
        }
        const auto *candidate = dynamic_cast<const Temp *>(phi.res.get());
        if (!candidate || candidate->type != Type::Int || phi.phiIncoming.size() != 2) {
            continue;
        }
        const Element *initial = nullptr;
        const Temp *backedge = nullptr;
        for (const auto &incoming: phi.phiIncoming) {
            if (incoming.predecessor == blocks[outsidePredecessors.front()]->label.nameAndId) {
                initial = incoming.value.get();
            } else if (incoming.predecessor == blocks[latches.front()]->label.nameAndId) {
                backedge = dynamic_cast<const Temp *>(incoming.value.get());
            }
        }
        const auto initialValue = constantValue(initial, function, defs);
        auto updateDefinition = backedge ? defs.find(backedge->id) : defs.end();
        if (!initialValue || !backedge || updateDefinition == defs.end()) {
            continue;
        }
        const Inst &update = blocks[updateDefinition->second.block]
                                     ->instructions[updateDefinition->second.instruction];
        std::optional<int> candidateStep;
        if (update.op == Op::Add) {
            if (sameTemp(update.arg1.get(), candidate->id)) {
                candidateStep = constantValue(update.arg2.get(), function, defs);
            } else if (sameTemp(update.arg2.get(), candidate->id)) {
                candidateStep = constantValue(update.arg1.get(), function, defs);
            }
        }
        if (candidateStep && *candidateStep == 1) {
            induction = candidate;
            start = *initialValue;
            step = *candidateStep;
            break;
        }
    }
    if (!induction || step != 1) {
        return std::nullopt;
    }

    std::optional<int> bound;
    for (const auto &inst: blocks[header]->instructions) {
        if (inst.op == Op::Leq && sameTemp(inst.arg1.get(), induction->id)) {
            bound = constantValue(inst.arg2.get(), function, defs);
            break;
        }
    }
    if (!bound || start > *bound) {
        return std::nullopt;
    }

    const Inst *store = nullptr;
    size_t storeBlock = 0;
    for (size_t block: loop) {
        for (const auto &inst: blocks[block]->instructions) {
            if (inst.op == Op::Call || inst.op == Op::GetString
                || inst.op == Op::Store || inst.op == Op::StoreDynamic
                || inst.op == Op::MemZero) {
                if (inst.op != Op::StoreDynamic || store) {
                    return std::nullopt;
                }
                store = &inst;
                storeBlock = block;
            }
        }
    }
    const auto *base = store ? dynamic_cast<const Var *>(store->arg1.get()) : nullptr;
    if (!store || !base || base->depth != 0 || base->storesAddress
        || base->dims.empty()
        || !cfg.dominates(storeBlock, latches.front())) {
        return std::nullopt;
    }

    auto offset = affineValue(store->arg2.get(), function, defs);
    auto value = affineValue(store->res.get(), function, defs);
    if (!offset || !value || offset->root != induction->id
        || (value->root && value->root != induction->id)) {
        return std::nullopt;
    }
    int elements = 1;
    for (int dimension: base->dims) {
        elements *= dimension;
    }
    const int iterations = *bound - start + 1;
    const int elementSize = sizeOfType(ptrToValue(base->type));
    const int firstOffset = wrappingAdd(
            wrappingMultiply(offset->coefficient, start), offset->constant);
    if (iterations != elements || firstOffset != 0
        || offset->coefficient != elementSize) {
        return std::nullopt;
    }
    return DenseArrayDefinition{*base, exit, *offset, *value};
}

bool mayClobberOnPath(
        const Function &function,
        const ControlFlowGraph &cfg,
        size_t start,
        size_t destination,
        size_t destinationInstruction,
        const Var &base) {
    const size_t blockCount = function.getBasicBlocks().size();
    std::vector<bool> reachableFromStart(blockCount, false);
    std::vector<size_t> work{start};
    reachableFromStart[start] = true;
    while (!work.empty()) {
        const size_t block = work.back();
        work.pop_back();
        for (size_t successor: cfg.successors[block]) {
            if (!reachableFromStart[successor]) {
                reachableFromStart[successor] = true;
                work.push_back(successor);
            }
        }
    }
    std::vector<bool> reachesDestination(blockCount, false);
    work = {destination};
    reachesDestination[destination] = true;
    while (!work.empty()) {
        const size_t block = work.back();
        work.pop_back();
        for (size_t predecessor: cfg.predecessors[block]) {
            if (!reachesDestination[predecessor]) {
                reachesDestination[predecessor] = true;
                work.push_back(predecessor);
            }
        }
    }

    const auto &blocks = function.getBasicBlocks();
    for (size_t block = 0; block < blockCount; ++block) {
        if (!reachableFromStart[block] || !reachesDestination[block]) {
            continue;
        }
        const size_t limit = block == destination
                                     ? destinationInstruction
                                     : blocks[block]->instructions.size();
        for (size_t instruction = 0; instruction < limit; ++instruction) {
            const Inst &inst = blocks[block]->instructions[instruction];
            if (inst.op == Op::Call) {
                return true;
            }
            if (inst.op != Op::Store && inst.op != Op::StoreDynamic
                && inst.op != Op::MemZero
                && inst.op != Op::GetString) {
                continue;
            }
            const auto *written = dynamic_cast<const Var *>(inst.arg1.get());
            if (!written || *written == base || written->storesAddress) {
                return true;
            }
        }
    }
    return false;
}

bool forwardDenseArrayValues(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto defs = collectTempDefinitions(function);
    const auto loops = collectNaturalLoops(cfg);
    std::vector<DenseArrayDefinition> arrays;
    for (const auto &[header, loop]: loops) {
        auto definition = denseArrayDefinition(function, cfg, header, loop, defs);
        if (definition) {
            arrays.push_back(std::move(*definition));
        }
    }
    if (arrays.empty()) {
        return false;
    }

    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        for (size_t instruction = 0;
             instruction < blocks[block]->instructions.size();
             ++instruction) {
            Inst &load = blocks[block]->instructions[instruction];
            if (load.op != Op::LoadDynamic) {
                continue;
            }
            const auto *base = dynamic_cast<const Var *>(load.arg1.get());
            const auto *result = dynamic_cast<const Temp *>(load.res.get());
            auto loadOffset = affineValue(load.arg2.get(), function, defs);
            if (!base || !result || !loadOffset || !loadOffset->root) {
                continue;
            }
            for (const auto &array: arrays) {
                if (!(array.base == *base) || !cfg.dominates(array.loopExit, block)
                    || loadOffset->coefficient != array.offset.coefficient
                    || loadOffset->constant != array.offset.constant
                    || mayClobberOnPath(function, cfg, array.loopExit, block,
                                        instruction, array.base)) {
                    continue;
                }

                const Temp root(*loadOffset->root, Type::Int);
                std::unique_ptr<Element> replacement;
                std::vector<Inst> generated;
                int id = nextTempId(function);
                if (array.value.coefficient == 0) {
                    const Temp value(id++, result->type);
                    generated.emplace_back(
                            Op::LoadImd,
                            value.clone(),
                            std::make_unique<ConstVal>(array.value.constant, result->type),
                            nullptr);
                    replacement = value.clone();
                } else if (array.value.coefficient == 1
                           && array.value.constant == 0) {
                    replacement = root.clone();
                } else {
                    const Temp scaled(id++, Type::Int);
                    generated.emplace_back(
                            Op::MulImd,
                            scaled.clone(),
                            root.clone(),
                            std::make_unique<ConstVal>(
                                    array.value.coefficient, Type::Int));
                    if (array.value.constant == 0) {
                        replacement = scaled.clone();
                    } else {
                        const Temp value(id++, Type::Int);
                        generated.emplace_back(
                                Op::Add,
                                value.clone(),
                                scaled.clone(),
                                std::make_unique<ConstVal>(
                                        array.value.constant, Type::Int));
                        replacement = value.clone();
                    }
                }
                function.replaceAllUsesWith(*result, *replacement);
                auto &instructions = blocks[block]->instructions;
                instructions.erase(instructions.begin()
                                   + static_cast<long>(instruction));
                instructions.insert(
                        instructions.begin() + static_cast<long>(instruction),
                        std::make_move_iterator(generated.begin()),
                        std::make_move_iterator(generated.end()));
                return true;
            }
        }
    }
    return false;
}

std::optional<MemoryLocation> exactLocation(const Inst &inst) {
    if (inst.op != Op::Load && inst.op != Op::Store) {
        return std::nullopt;
    }
    const auto *base = dynamic_cast<const Var *>(inst.arg1.get());
    // For an array parameter, Load with no offset reads the pointer slot while
    // Store writes through that pointer. They are different memory locations.
    if (!base || base->storesAddress) {
        return std::nullopt;
    }
    int offset = 0;
    if (inst.arg2) {
        const auto *constant = dynamic_cast<const ConstVal *>(inst.arg2.get());
        if (!constant) {
            return std::nullopt;
        }
        offset = constant->value;
    }
    return MemoryLocation{*base, offset};
}

void killBase(MemoryState &state, const Var &base) {
    for (auto value = state.begin(); value != state.end();) {
        if (value->first.base == base) {
            value = state.erase(value);
        } else {
            ++value;
        }
    }
}

void transfer(const Inst &inst, MemoryState &state) {
    if (inst.op == Op::Call) {
        state.clear();
        return;
    }
    if (inst.op == Op::StoreDynamic || inst.op == Op::MemZero
        || inst.op == Op::GetString) {
        const auto *base = dynamic_cast<const Var *>(inst.arg1.get());
        if (base) {
            killBase(state, *base);
        } else {
            state.clear();
        }
        return;
    }
    const auto location = exactLocation(inst);
    if (!location) {
        return;
    }
    if (inst.op == Op::Store) {
        const auto *value = dynamic_cast<const Temp *>(inst.res.get());
        if (value && value->id >= 0) {
            state[*location] = {value->id, value->type};
        } else {
            state.erase(*location);
        }
    } else if (inst.op == Op::Load) {
        const auto *result = dynamic_cast<const Temp *>(inst.res.get());
        if (result && result->id >= 0 && state.find(*location) == state.end()) {
            state[*location] = {result->id, result->type};
        }
    }
}

MemoryState meet(const std::vector<size_t> &predecessors,
                 const std::vector<MemoryState> &outStates) {
    if (predecessors.empty()) {
        return {};
    }
    MemoryState result = outStates[predecessors.front()];
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

bool eliminateOverwrittenStores(Function &function) {
    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        std::unordered_map<MemoryLocation, size_t, MemoryLocationHash> pending;
        std::vector<bool> remove(block->instructions.size(), false);
        for (size_t index = 0; index < block->instructions.size(); ++index) {
            const auto &inst = block->instructions[index];
            if (inst.op == Op::Call) {
                pending.clear();
                continue;
            }
            if (inst.op == Op::LoadPtr) {
                pending.clear();
                continue;
            }
            if (inst.op == Op::StoreDynamic || inst.op == Op::MemZero
                || inst.op == Op::LoadDynamic
                || inst.op == Op::GetString || inst.op == Op::PrintStr) {
                const auto *base = dynamic_cast<const Var *>(inst.arg1.get());
                if (!base) {
                    pending.clear();
                } else {
                    for (auto value = pending.begin(); value != pending.end();) {
                        if (value->first.base == *base) {
                            value = pending.erase(value);
                        } else {
                            ++value;
                        }
                    }
                }
                continue;
            }
            const auto location = exactLocation(inst);
            if (!location) {
                continue;
            }
            if (inst.op == Op::Load) {
                pending.erase(*location);
            } else {
                auto previous = pending.find(*location);
                if (previous != pending.end()) {
                    remove[previous->second] = true;
                    changed = true;
                }
                pending[*location] = index;
            }
        }
        if (std::find(remove.begin(), remove.end(), true) != remove.end()) {
            std::vector<Inst> kept;
            kept.reserve(block->instructions.size());
            for (size_t index = 0; index < block->instructions.size(); ++index) {
                if (!remove[index]) {
                    kept.push_back(std::move(block->instructions[index]));
                }
            }
            block->instructions = std::move(kept);
        }
    }
    return changed;
}

} // namespace

bool optimizeMemoryValues(Function &function) {
    if (forwardDenseArrayValues(function)) {
        return true;
    }
    const auto cfg = buildControlFlowGraph(function);
    const size_t blockCount = function.getBasicBlocks().size();
    if (blockCount == 0) {
        return false;
    }
    std::vector<MemoryState> inStates(blockCount);
    std::vector<MemoryState> outStates(blockCount);
    bool dataflowChanged = true;
    while (dataflowChanged) {
        dataflowChanged = false;
        for (size_t block: cfg.reversePostOrder) {
            MemoryState nextIn = block == 0 ? MemoryState{}
                                            : meet(cfg.predecessors[block], outStates);
            MemoryState nextOut = nextIn;
            for (const auto &inst: function.getBasicBlocks()[block]->instructions) {
                transfer(inst, nextOut);
            }
            if (nextIn != inStates[block] || nextOut != outStates[block]) {
                inStates[block] = std::move(nextIn);
                outStates[block] = std::move(nextOut);
                dataflowChanged = true;
            }
        }
    }

    std::unordered_map<int, AvailableValue> replacements;
    auto resolve = [&](AvailableValue value) {
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

    bool changed = false;
    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block < blockCount; ++block) {
        MemoryState state = inStates[block];
        auto &instructions = blocks[block]->instructions;
        for (size_t index = 0; index < instructions.size();) {
            auto &inst = instructions[index];
            const auto location = exactLocation(inst);
            const auto *destination = dynamic_cast<const Temp *>(inst.res.get());
            if (inst.op == Op::Load && location && destination && destination->id >= 0) {
                auto available = state.find(*location);
                if (available != state.end()) {
                    const AvailableValue replacement = resolve(available->second);
                    if (replacement.temp != destination->id
                        && replacement.type == destination->type) {
                        replacements[destination->id] = replacement;
                        state[*location] = replacement;
                        instructions.erase(instructions.begin() + static_cast<long>(index));
                        changed = true;
                        continue;
                    }
                }
            }
            transfer(inst, state);
            ++index;
        }
    }

    if (!replacements.empty()) {
        for (auto &block: blocks) {
            for (auto &inst: block->instructions) {
                const auto operands = inst.operands();
                for (const auto &operand: operands) {
                    if (operand.role == OperandRole::Definition) {
                        continue;
                    }
                    const auto *temp = dynamic_cast<const Temp *>(operand.value);
                    auto replacement = temp ? replacements.find(temp->id) : replacements.end();
                    if (replacement == replacements.end()) {
                        continue;
                    }
                    const AvailableValue resolved = resolve(replacement->second);
                    inst.setOperand(
                            operand.slot,
                            std::make_unique<Temp>(resolved.temp, resolved.type));
                }
            }
        }
    }
    return eliminateOverwrittenStores(function) || changed;
}

} // namespace IR
