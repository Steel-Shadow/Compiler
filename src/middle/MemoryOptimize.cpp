#include "middle/Optimize.h"

#include "middle/Analysis.h"

#include <algorithm>
#include <optional>
#include <string>
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
    if (inst.op == Op::StoreDynamic || inst.op == Op::GetString) {
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
            if (inst.op == Op::StoreDynamic || inst.op == Op::LoadDynamic
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
