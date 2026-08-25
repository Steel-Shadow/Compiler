//
// Created by Steel_Shadow on 2023/11/15.
//

#include "Register.h"

#include "errorHandler/Error.h"
#include "Instruction.h"
#include "Memory.h"
#include "middle/Analysis.h"
#include "MIPS.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace MIPS;

std::map<int, Register> MIPS::tempToRegs;
std::map<IR::Var, Register> MIPS::varToRegs;
std::map<IR::Var, Register> MIPS::allocatedVarRegs;

namespace {

using TempGraph = std::unordered_map<int, std::unordered_set<int>>;
using VarGraph = std::unordered_map<IR::Var, std::unordered_set<IR::Var>>;
using MoveAffinity = std::unordered_map<int, std::unordered_set<int>>;

std::unordered_set<int> spilledTemps;
std::unordered_map<int, int> rematerializedConstants;
std::unordered_map<int, IR::Var> coalescedLoadVariables;
std::unordered_map<int, IR::Var> coalescedStoreVariables;
std::unordered_map<const IR::Inst *, IR::TempSet> liveTempsAfter;
std::unordered_map<int, Register> instructionScratchRegs;
std::vector<Register> freeScratchRegs;
bool restoreReturnAddress = false;
std::vector<Register> savedVariableRegs;
std::unordered_set<std::string> registerArgumentFunctions;

template<class Node>
using Graph = std::unordered_map<Node, std::unordered_set<Node>>;

template<class Node>
void addEdge(Graph<Node> &graph, const Node &lhs, const Node &rhs) {
    if (lhs == rhs) {
        return;
    }
    graph[lhs].insert(rhs);
    graph[rhs].insert(lhs);
}

template<class Node>
void addClique(Graph<Node> &graph, const std::unordered_set<Node> &nodes) {
    for (auto lhs = nodes.begin(); lhs != nodes.end(); ++lhs) {
        auto rhs = lhs;
        for (++rhs; rhs != nodes.end(); ++rhs) {
            addEdge(graph, *lhs, *rhs);
        }
    }
}

template<class Node>
std::unordered_map<Node, int> colorGraph(
        const Graph<Node> &original,
        const std::unordered_map<Node, int> &weights,
        int colorCount) {
    Graph<Node> graph = original;
    std::vector<Node> stack;
    stack.reserve(graph.size());

    while (!graph.empty()) {
        auto selected = graph.end();
        for (auto iter = graph.begin(); iter != graph.end(); ++iter) {
            if (static_cast<int>(iter->second.size()) >= colorCount) {
                continue;
            }
            if (selected == graph.end() || std::less<Node>{}(iter->first, selected->first)) {
                selected = iter;
            }
        }

        if (selected == graph.end()) {
            for (auto iter = graph.begin(); iter != graph.end(); ++iter) {
                if (selected == graph.end()) {
                    selected = iter;
                    continue;
                }
                const long long lhsWeight = weights.count(iter->first) ? weights.at(iter->first) : 1;
                const long long rhsWeight = weights.count(selected->first) ? weights.at(selected->first) : 1;
                const long long lhsScore = lhsWeight * static_cast<long long>(selected->second.size() + 1);
                const long long rhsScore = rhsWeight * static_cast<long long>(iter->second.size() + 1);
                if (lhsScore < rhsScore
                    || (lhsScore == rhsScore && std::less<Node>{}(iter->first, selected->first))) {
                    selected = iter;
                }
            }
        }

        Node node = selected->first;
        stack.push_back(node);
        for (const auto &neighbor: selected->second) {
            auto other = graph.find(neighbor);
            if (other != graph.end()) {
                other->second.erase(node);
            }
        }
        graph.erase(selected);
    }

    std::unordered_map<Node, int> colors;
    while (!stack.empty()) {
        Node node = stack.back();
        stack.pop_back();
        std::vector<bool> unavailable(static_cast<size_t>(colorCount), false);
        auto neighbors = original.find(node);
        if (neighbors != original.end()) {
            for (const auto &neighbor: neighbors->second) {
                auto color = colors.find(neighbor);
                if (color != colors.end()) {
                    unavailable[static_cast<size_t>(color->second)] = true;
                }
            }
        }
        for (int color = 0; color < colorCount; ++color) {
            if (!unavailable[static_cast<size_t>(color)]) {
                colors.emplace(node, color);
                break;
            }
        }
    }
    return colors;
}

std::unordered_map<int, int> colorTempGraph(
        const TempGraph &original,
        const std::unordered_map<int, int> &weights,
        const std::vector<Register> &registers,
        const IR::TempSet &liveAcrossCalls,
        const MoveAffinity &affinities,
        const std::unordered_set<int> &forcedSpills = {}) {
    TempGraph graph = original;
    for (int spilled: forcedSpills) {
        auto node = graph.find(spilled);
        if (node == graph.end()) {
            continue;
        }
        for (int neighbor: node->second) {
            auto other = graph.find(neighbor);
            if (other != graph.end()) {
                other->second.erase(spilled);
            }
        }
        graph.erase(node);
    }
    std::vector<int> stack;
    stack.reserve(graph.size());
    const int colorCount = static_cast<int>(registers.size());

    while (!graph.empty()) {
        auto selected = graph.end();
        for (auto candidate = graph.begin(); candidate != graph.end(); ++candidate) {
            if (static_cast<int>(candidate->second.size()) >= colorCount) {
                continue;
            }
            if (selected == graph.end() || candidate->first < selected->first) {
                selected = candidate;
            }
        }
        if (selected == graph.end()) {
            long long bestScore = std::numeric_limits<long long>::max();
            for (auto candidate = graph.begin(); candidate != graph.end(); ++candidate) {
                const long long weight = weights.count(candidate->first)
                                                 ? weights.at(candidate->first)
                                                 : 1;
                const long long score = weight
                                        / static_cast<long long>(candidate->second.size() + 1);
                if (selected == graph.end() || score < bestScore
                    || (score == bestScore && candidate->first < selected->first)) {
                    selected = candidate;
                    bestScore = score;
                }
            }
        }
        const int node = selected->first;
        stack.push_back(node);
        for (int neighbor: selected->second) {
            auto other = graph.find(neighbor);
            if (other != graph.end()) {
                other->second.erase(node);
            }
        }
        graph.erase(selected);
    }

    std::unordered_map<int, int> colors;
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        std::vector<bool> unavailable(registers.size(), false);
        auto neighbors = original.find(node);
        if (neighbors != original.end()) {
            for (int neighbor: neighbors->second) {
                auto color = colors.find(neighbor);
                if (color != colors.end()) {
                    unavailable[static_cast<size_t>(color->second)] = true;
                }
            }
        }

        int selectedColor = -1;
        int selectedScore = std::numeric_limits<int>::min();
        for (int color = 0; color < colorCount; ++color) {
            if (unavailable[static_cast<size_t>(color)]) {
                continue;
            }
            const Register reg = registers[static_cast<size_t>(color)];
            const bool calleeSaved = reg >= Register::s0 && reg <= Register::s7;
            int score = liveAcrossCalls.find(node) != liveAcrossCalls.end()
                                ? (calleeSaved ? 20 : 0)
                                : (calleeSaved ? 0 : 20);
            auto affinity = affinities.find(node);
            if (affinity != affinities.end()) {
                for (int related: affinity->second) {
                    auto relatedColor = colors.find(related);
                    if (relatedColor != colors.end() && relatedColor->second == color) {
                        score += 100;
                    }
                }
            }
            if (selectedColor < 0 || score > selectedScore) {
                selectedColor = color;
                selectedScore = score;
            }
        }
        if (selectedColor >= 0) {
            colors.emplace(node, selectedColor);
        }
    }
    return colors;
}

TempGraph buildTempInterference(const IR::Function &function,
                                const IR::TempLiveness &liveness,
                                const std::vector<size_t> &loopDepth,
                                std::unordered_map<int, int> &weights,
                                MoveAffinity &affinities) {
    TempGraph graph;
    for (size_t blockIndex = 0; blockIndex < function.getBasicBlocks().size(); ++blockIndex) {
        const auto &block = function.getBasicBlocks()[blockIndex];
        const int blockWeight = 1 << std::min<size_t>(loopDepth[blockIndex], 6);
        for (const auto &inst: block->instructions) {
            for (int used: IR::usedTemps(inst)) {
                graph[used];
                weights[used] += blockWeight;
            }
            auto definition = IR::definedTemp(inst);
            if (!definition) {
                continue;
            }
            graph[*definition];
            weights[*definition] += blockWeight;
            auto live = liveness.liveAfter.find(&inst);
            if (live == liveness.liveAfter.end()) {
                continue;
            }
            std::optional<int> moveSource;
            if (inst.op == IR::Op::NewMove) {
                const auto *source = dynamic_cast<const IR::Temp *>(inst.arg1.get());
                if (source && source->id >= 0) {
                    moveSource = source->id;
                    affinities[*definition].insert(source->id);
                    affinities[source->id].insert(*definition);
                }
            }
            for (int other: live->second) {
                if (!moveSource || other != *moveSource) {
                    addEdge(graph, *definition, other);
                }
            }
        }
    }
    for (const auto &live: liveness.liveIn) {
        addClique(graph, live);
    }
    for (const auto &live: liveness.liveOut) {
        addClique(graph, live);
    }
    return graph;
}

VarGraph buildVariableInterference(const IR::Function &function,
                                   const IR::VariableLiveness &liveness,
                                   const std::vector<size_t> &loopDepth,
                                   std::unordered_map<IR::Var, int> &weights) {
    VarGraph graph;
    for (const auto &candidate: liveness.candidates) {
        graph[candidate];
        weights[candidate] = 1;
    }
    for (size_t blockIndex = 0; blockIndex < function.getBasicBlocks().size(); ++blockIndex) {
        const auto &block = function.getBasicBlocks()[blockIndex];
        const int blockWeight = 1 << std::min<size_t>(loopDepth[blockIndex], 6);
        for (const auto &inst: block->instructions) {
            const auto *var = dynamic_cast<const IR::Var *>(inst.arg1.get());
            if (!var || liveness.candidates.find(*var) == liveness.candidates.end()) {
                continue;
            }
            if (inst.op == IR::Op::Load || inst.op == IR::Op::Store) {
                weights[*var] += blockWeight;
            }
            if (inst.op != IR::Op::Store) {
                continue;
            }
            auto live = liveness.liveAfter.find(&inst);
            if (live == liveness.liveAfter.end()) {
                continue;
            }
            for (const auto &other: live->second) {
                addEdge(graph, *var, other);
            }
        }
    }
    for (const auto &live: liveness.liveIn) {
        addClique(graph, live);
    }
    for (const auto &live: liveness.liveOut) {
        addClique(graph, live);
    }
    return graph;
}

Register scratchRegisterFor(const IR::Temp *temp, bool load) {
    auto existing = instructionScratchRegs.find(temp->id);
    if (existing != instructionScratchRegs.end()) {
        return existing->second;
    }
    if (freeScratchRegs.empty()) {
        Error::raise("MIPS scratch register pool exhausted");
        return Register::t9;
    }
    const Register reg = freeScratchRegs.back();
    freeScratchRegs.pop_back();
    instructionScratchRegs.emplace(temp->id, reg);
    if (load) {
        auto rematerialized = rematerializedConstants.find(temp->id);
        if (rematerialized != rematerializedConstants.end()) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(
                    Op::li, reg, Register::none, rematerialized->second));
            return reg;
        }
        auto offset = StackMemory::tempToOffset.find(temp->id);
        if (offset == StackMemory::tempToOffset.end()) {
            Error::raise("Missing stack slot for spilled temporary");
        } else {
            assemblies.push_back(std::make_unique<I_imm_Inst>(
                    Op::lw, reg, Register::sp, -offset->second));
        }
    }
    return reg;
}

int tempColor(Register reg) {
    return static_cast<int>(reg) - static_cast<int>(Register::t0);
}

int variableColor(Register reg) {
    return static_cast<int>(reg) - static_cast<int>(Register::s0);
}

bool isTemporaryRegister(Register reg) {
    return reg >= Register::t0 && reg <= Register::t6;
}

bool isRegisterArgumentCandidate(const IR::Function &function) {
    if (function.getParams().size() > 4) {
        return false;
    }
    return std::none_of(function.getBasicBlocks().begin(), function.getBasicBlocks().end(), [](const auto &block) {
        return std::any_of(block->instructions.begin(), block->instructions.end(), [](const IR::Inst &inst) {
            return inst.op == IR::Op::Call || inst.op == IR::Op::GetInt
                   || inst.op == IR::Op::GetChar || inst.op == IR::Op::GetString
                   || inst.op == IR::Op::PrintInt || inst.op == IR::Op::PrintChar
                   || inst.op == IR::Op::PrintStr;
        });
    });
}

bool callCanPassArgumentsDirectly(const IR::BasicBlock &block,
                                  size_t callIndex,
                                  size_t parameterCount) {
    const auto &instructions = block.instructions;
    size_t frameStart = callIndex;
    size_t nestedFrames = 0;
    for (size_t reverse = callIndex; reverse-- > 0;) {
        if (instructions[reverse].op == IR::Op::OutStack) {
            ++nestedFrames;
        } else if (instructions[reverse].op == IR::Op::InStack) {
            if (nestedFrames == 0) {
                frameStart = reverse;
                break;
            }
            --nestedFrames;
        }
    }
    if (frameStart == callIndex) {
        return false;
    }

    size_t pushes = 0;
    for (size_t index = frameStart + 1; index < callIndex; ++index) {
        if (instructions[index].op == IR::Op::Call) {
            return false;
        }
        if (instructions[index].op == IR::Op::PushParam
            || instructions[index].op == IR::Op::PushAddressParam) {
            ++pushes;
        }
    }
    return pushes == parameterCount;
}

} // namespace

void MIPS::configureRegisterArgumentConvention(const IR::Module &module) {
    registerArgumentFunctions.clear();
    std::unordered_map<std::string, size_t> parameterCounts;
    for (const auto &function: module.getFunctions()) {
        if (isRegisterArgumentCandidate(*function)) {
            registerArgumentFunctions.insert(function->getName());
            parameterCounts.emplace(function->getName(), function->getParams().size());
        }
    }

    std::unordered_set<std::string> ineligible;
    auto inspectFunction = [&](const IR::Function &function) {
        for (const auto &block: function.getBasicBlocks()) {
            for (size_t index = 0; index < block->instructions.size(); ++index) {
                const auto &inst = block->instructions[index];
                const auto *callee = dynamic_cast<const IR::Label *>(inst.arg1.get());
                if (inst.op != IR::Op::Call || !callee
                    || registerArgumentFunctions.find(callee->nameAndId)
                               == registerArgumentFunctions.end()) {
                    continue;
                }
                if (!callCanPassArgumentsDirectly(
                            *block, index, parameterCounts.at(callee->nameAndId))) {
                    ineligible.insert(callee->nameAndId);
                }
            }
        }
    };
    inspectFunction(module.getMainFunction());
    for (const auto &function: module.getFunctions()) {
        inspectFunction(*function);
    }
    for (const auto &name: ineligible) {
        registerArgumentFunctions.erase(name);
    }
}

void MIPS::prepareRegisterAllocation(const IR::Function &function) {
    tempToRegs.clear();
    varToRegs.clear();
    allocatedVarRegs.clear();
    spilledTemps.clear();
    rematerializedConstants.clear();
    coalescedLoadVariables.clear();
    coalescedStoreVariables.clear();
    instructionScratchRegs.clear();
    liveTempsAfter.clear();
    savedVariableRegs.clear();
    restoreReturnAddress = false;

    const auto cfg = IR::buildControlFlowGraph(function);
    const auto loopDepth = IR::computeLoopDepths(cfg);
    const auto tempLiveness = IR::analyzeTempLiveness(function, cfg);
    const auto variableLiveness = IR::analyzeVariableLiveness(function, cfg);
    liveTempsAfter = tempLiveness.liveAfter;

    std::unordered_map<int, int> definitionCounts;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (auto definition = IR::definedTemp(inst)) {
                ++definitionCounts[*definition];
            }
        }
    }
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != IR::Op::LoadImd) {
                continue;
            }
            const auto *temp = dynamic_cast<const IR::Temp *>(inst.res.get());
            const auto *constant = dynamic_cast<const IR::ConstVal *>(inst.arg1.get());
            if (temp && temp->id >= 0 && constant && definitionCounts[temp->id] == 1) {
                rematerializedConstants[temp->id] = constant->value;
            }
        }
    }

    std::unordered_map<IR::Var, int> variableWeights;
    auto variableGraph = buildVariableInterference(
            function, variableLiveness, loopDepth, variableWeights);
    std::unordered_set<IR::Var> parameterVariables;
    for (const auto &param: function.getParams()) {
        IR::Var var(param.name, 1, false, param.dims, param.type, !param.dims.empty());
        if (variableGraph.find(var) != variableGraph.end()) {
            parameterVariables.insert(std::move(var));
        }
    }
    addClique(variableGraph, parameterVariables);
    const auto variableColors = colorGraph(variableGraph, variableWeights, MAX_VAR_REGS);
    for (const auto &[var, color]: variableColors) {
        allocatedVarRegs[var] = static_cast<Register>(static_cast<int>(Register::s0) + color);
    }
    if (usesRegisterArguments(function)) {
        for (size_t index = 0; index < function.getParams().size(); ++index) {
            const auto &param = function.getParams()[index];
            allocatedVarRegs[IR::Var(
                    param.name, 1, false, param.dims, param.type,
                    !param.dims.empty())] =
                    argumentRegister(index);
        }
    }

    std::unordered_set<Register> reservedCalleeSaved;
    for (const auto &[var, reg]: allocatedVarRegs) {
        (void) var;
        if (reg >= Register::s0 && reg <= Register::s7) {
            reservedCalleeSaved.insert(reg);
        }
    }
    std::vector<Register> allocatableRegisters = {
            Register::t0,
            Register::t1,
            Register::t2,
            Register::t3,
            Register::t4,
            Register::t5,
            Register::t6,
    };
    for (Register reg: {Register::s0, Register::s1, Register::s2, Register::s3,
                        Register::s4, Register::s5, Register::s6, Register::s7}) {
        if (reservedCalleeSaved.find(reg) == reservedCalleeSaved.end()) {
            allocatableRegisters.push_back(reg);
        }
    }

    IR::TempSet liveAcrossCalls;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if (inst.op != IR::Op::Call) {
                continue;
            }
            auto live = tempLiveness.liveAfter.find(&inst);
            if (live == tempLiveness.liveAfter.end()) {
                continue;
            }
            for (int temp: live->second) {
                liveAcrossCalls.insert(temp);
            }
        }
    }

    std::unordered_map<int, int> tempWeights;
    MoveAffinity affinities;
    const auto tempGraph = buildTempInterference(
            function, tempLiveness, loopDepth, tempWeights, affinities);
    for (const auto &[temp, value]: rematerializedConstants) {
        (void) value;
        tempWeights[temp] = 0;
    }
    auto tempColors = colorTempGraph(
            tempGraph, tempWeights, allocatableRegisters, liveAcrossCalls, affinities);
    const auto estimatedSpillCost = [&](const std::unordered_map<int, int> &colors) {
        long long cost = 0;
        for (const auto &[temp, neighbors]: tempGraph) {
            (void) neighbors;
            const bool forcedAcrossCall = liveAcrossCalls.find(temp) != liveAcrossCalls.end()
                                          && rematerializedConstants.find(temp)
                                                     != rematerializedConstants.end();
            if (colors.find(temp) != colors.end() && !forcedAcrossCall) {
                continue;
            }
            const long long weight = tempWeights.count(temp) != 0
                                             ? tempWeights.at(temp)
                                             : 1;
            cost += rematerializedConstants.find(temp) != rematerializedConstants.end()
                            ? weight
                            : 2 * weight;
        }
        return cost;
    };
    const bool spillsComputedValue = std::any_of(
            tempGraph.begin(), tempGraph.end(), [&](const auto &node) {
                return tempColors.find(node.first) == tempColors.end()
                       && rematerializedConstants.find(node.first)
                                  == rematerializedConstants.end();
            });
    if (spillsComputedValue && !rematerializedConstants.empty()) {
        std::unordered_set<int> rematerialized;
        for (const auto &[temp, value]: rematerializedConstants) {
            (void) value;
            rematerialized.insert(temp);
        }
        auto alternative = colorTempGraph(
                tempGraph, tempWeights, allocatableRegisters,
                liveAcrossCalls, affinities, rematerialized);
        if (estimatedSpillCost(alternative) < estimatedSpillCost(tempColors)) {
            tempColors = std::move(alternative);
        }
    }
    for (const auto &[temp, neighbors]: tempGraph) {
        (void) neighbors;
        auto color = tempColors.find(temp);
        if (color == tempColors.end()) {
            spilledTemps.insert(temp);
        } else {
            tempToRegs[temp] = allocatableRegisters[static_cast<size_t>(color->second)];
        }
    }
    for (int temp: liveAcrossCalls) {
        if (rematerializedConstants.find(temp) != rematerializedConstants.end()) {
            tempToRegs.erase(temp);
            spilledTemps.insert(temp);
        }
    }

    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            if ((inst.op != IR::Op::Load && inst.op != IR::Op::Parameter) || inst.arg2) {
                continue;
            }
            const auto *temp = dynamic_cast<const IR::Temp *>(inst.res.get());
            const auto *var = dynamic_cast<const IR::Var *>(inst.arg1.get());
            if (!temp || temp->id < 0 || !var || !IR::isRegisterCandidate(*var)
                || definitionCounts[temp->id] != 1
                || allocatedVarRegs.find(*var) == allocatedVarRegs.end()) {
                continue;
            }

            const Register preferred = allocatedVarRegs.at(*var);
            bool safe = true;
            for (const auto &candidateBlock: function.getBasicBlocks()) {
                for (const auto &candidate: candidateBlock->instructions) {
                    auto live = tempLiveness.liveAfter.find(&candidate);
                    if (live == tempLiveness.liveAfter.end()
                        || live->second.find(temp->id) == live->second.end()
                        || candidate.op != IR::Op::Store) {
                        continue;
                    }
                    const auto *storedVar = dynamic_cast<const IR::Var *>(candidate.arg1.get());
                    const auto *storedValue = dynamic_cast<const IR::Temp *>(candidate.res.get());
                    auto allocation = storedVar ? allocatedVarRegs.find(*storedVar) : allocatedVarRegs.end();
                    if (allocation != allocatedVarRegs.end() && allocation->second == preferred
                        && (!storedValue || storedValue->id != temp->id)) {
                        safe = false;
                        break;
                    }
                }
                if (!safe) {
                    break;
                }
            }
            if (safe) {
                coalescedLoadVariables.emplace(temp->id, *var);
            }
        }
    }

    if (usesRegisterArguments(function)) {
        bool propagated = true;
        while (propagated) {
            propagated = false;
            for (const auto &block: function.getBasicBlocks()) {
                for (const auto &inst: block->instructions) {
                    if (inst.op != IR::Op::NewMove) {
                        continue;
                    }
                    const auto *destination = dynamic_cast<const IR::Temp *>(inst.res.get());
                    const auto *source = dynamic_cast<const IR::Temp *>(inst.arg1.get());
                    auto sourceVariable = source
                                                  ? coalescedLoadVariables.find(source->id)
                                                  : coalescedLoadVariables.end();
                    if (!destination || destination->id < 0 || !source || source->id < 0
                        || sourceVariable == coalescedLoadVariables.end()
                        || coalescedLoadVariables.find(destination->id)
                                   != coalescedLoadVariables.end()) {
                        continue;
                    }

                    const Register preferred = allocatedVarRegs.at(sourceVariable->second);
                    if (preferred < Register::a0 || preferred > Register::a3) {
                        continue;
                    }
                    bool interferes = false;
                    auto neighbors = tempGraph.find(destination->id);
                    if (neighbors != tempGraph.end()) {
                        for (const auto &[temp, var]: coalescedLoadVariables) {
                            if (var == sourceVariable->second
                                && neighbors->second.find(temp) != neighbors->second.end()) {
                                interferes = true;
                                break;
                            }
                        }
                    }
                    if (!interferes) {
                        coalescedLoadVariables.emplace(
                                destination->id, sourceVariable->second);
                        propagated = true;
                    }
                }
            }
        }
    }

    struct DefinitionLocation {
        size_t block{};
        size_t instruction{};
    };
    std::unordered_map<int, DefinitionLocation> definitions;
    std::unordered_map<int, int> useCounts;
    for (size_t block = 0; block < function.getBasicBlocks().size(); ++block) {
        const auto &instructions = function.getBasicBlocks()[block]->instructions;
        for (size_t index = 0; index < instructions.size(); ++index) {
            if (auto definition = IR::definedTemp(instructions[index])) {
                definitions[*definition] = {block, index};
            }
            for (int used: IR::usedTemps(instructions[index])) {
                ++useCounts[used];
            }
        }
    }

    for (size_t block = 0; block < function.getBasicBlocks().size(); ++block) {
        const auto &instructions = function.getBasicBlocks()[block]->instructions;
        for (size_t storeIndex = 0; storeIndex < instructions.size(); ++storeIndex) {
            const auto &store = instructions[storeIndex];
            const auto *temp = dynamic_cast<const IR::Temp *>(store.res.get());
            const auto *var = dynamic_cast<const IR::Var *>(store.arg1.get());
            if (store.op != IR::Op::Store || store.arg2 || !temp || temp->id < 0 || !var
                || var->storesAddress
                || useCounts[temp->id] != 1 || allocatedVarRegs.find(*var) == allocatedVarRegs.end()) {
                continue;
            }
            auto definition = definitions.find(temp->id);
            if (definition == definitions.end() || definition->second.block != block
                || definition->second.instruction >= storeIndex) {
                continue;
            }

            const Register preferred = allocatedVarRegs.at(*var);
            bool safe = true;
            for (size_t index = definition->second.instruction + 1; index < storeIndex; ++index) {
                const auto *loaded = dynamic_cast<const IR::Var *>(instructions[index].arg1.get());
                if (instructions[index].op == IR::Op::Load && loaded && *loaded == *var) {
                    safe = false;
                    break;
                }
            }
            auto liveTemps = tempLiveness.liveAfter.find(
                    &instructions[definition->second.instruction]);
            if (safe && liveTemps != tempLiveness.liveAfter.end()) {
                for (int live: liveTemps->second) {
                    auto load = coalescedLoadVariables.find(live);
                    if (load != coalescedLoadVariables.end()
                        && allocatedVarRegs.at(load->second) == preferred) {
                        safe = false;
                        break;
                    }
                }
            }
            auto liveVars = variableLiveness.liveAfter.find(
                    &instructions[definition->second.instruction]);
            if (safe && liveVars != variableLiveness.liveAfter.end()) {
                for (const auto &live: liveVars->second) {
                    auto allocation = allocatedVarRegs.find(live);
                    if (!(live == *var) && allocation != allocatedVarRegs.end()
                        && allocation->second == preferred) {
                        safe = false;
                        break;
                    }
                }
            }
            if (safe) {
                coalescedStoreVariables.emplace(temp->id, *var);
            }
        }
    }
}

void MIPS::reserveSpillSlots() {
    std::vector<int> ordered(spilledTemps.begin(), spilledTemps.end());
    std::sort(ordered.begin(), ordered.end());
    for (int temp: ordered) {
        if (rematerializedConstants.find(temp) != rematerializedConstants.end()) {
            continue;
        }
        StackMemory::curOffset += wordSize;
        StackMemory::tempToOffset[temp] = StackMemory::curOffset;
    }
}

void MIPS::beginInstruction(const IR::Inst &) {
    instructionScratchRegs.clear();
    freeScratchRegs = {Register::t9, Register::t8, Register::t7};
}

void MIPS::endInstruction() {
    instructionScratchRegs.clear();
    freeScratchRegs.clear();
}

Register MIPS::newReg(const IR::Temp *temp) {
    if (temp->id < 0) {
        return static_cast<Register>(-temp->id);
    }
    auto coalesced = coalescedLoadVariables.find(temp->id);
    if (coalesced != coalescedLoadVariables.end()) {
        return allocatedVarRegs.at(coalesced->second);
    }
    auto store = coalescedStoreVariables.find(temp->id);
    if (store != coalescedStoreVariables.end()) {
        return allocatedVarRegs.at(store->second);
    }
    auto allocated = tempToRegs.find(temp->id);
    if (allocated != tempToRegs.end()) {
        return allocated->second;
    }
    return scratchRegisterFor(temp, false);
}

Register MIPS::getReg(const IR::Temp *temp) {
    if (temp->id < 0) {
        return static_cast<Register>(-temp->id);
    }
    auto coalesced = coalescedLoadVariables.find(temp->id);
    if (coalesced != coalescedLoadVariables.end()) {
        return allocatedVarRegs.at(coalesced->second);
    }
    auto store = coalescedStoreVariables.find(temp->id);
    if (store != coalescedStoreVariables.end()) {
        return allocatedVarRegs.at(store->second);
    }
    auto allocated = tempToRegs.find(temp->id);
    if (allocated != tempToRegs.end()) {
        return allocated->second;
    }
    return scratchRegisterFor(temp, true);
}

Register MIPS::acquireScratchRegister() {
    if (freeScratchRegs.empty()) {
        Error::raise("MIPS scratch register pool exhausted");
        return Register::t9;
    }
    const Register reg = freeScratchRegs.back();
    freeScratchRegs.pop_back();
    return reg;
}

std::optional<int> MIPS::knownConstant(const IR::Element *element) {
    if (const auto *constant = dynamic_cast<const IR::ConstVal *>(element)) {
        return constant->value;
    }
    const auto *temp = dynamic_cast<const IR::Temp *>(element);
    if (!temp) {
        return std::nullopt;
    }
    auto constant = rematerializedConstants.find(temp->id);
    if (constant == rematerializedConstants.end()) {
        return std::nullopt;
    }
    return constant->second;
}

int MIPS::tempSaveOffset(Register reg) {
    return wordSize * (2 + tempColor(reg));
}

int MIPS::variableSaveOffset(Register reg) {
    return wordSize * (2 + MAX_TEMP_REGS + variableColor(reg));
}

std::vector<Register> MIPS::liveTempRegistersAfter(const IR::Inst &inst) {
    std::set<Register> registers;
    auto live = liveTempsAfter.find(&inst);
    if (live != liveTempsAfter.end()) {
        for (int temp: live->second) {
            auto coalesced = coalescedLoadVariables.find(temp);
            auto store = coalescedStoreVariables.find(temp);
            if (coalesced != coalescedLoadVariables.end()
                || store != coalescedStoreVariables.end()) {
                continue;
            }
            auto allocated = tempToRegs.find(temp);
            if (allocated != tempToRegs.end() && isTemporaryRegister(allocated->second)) {
                registers.insert(allocated->second);
            }
        }
    }
    return {registers.begin(), registers.end()};
}

std::vector<Register> MIPS::usedVariableRegisters() {
    std::set<Register> registers;
    for (const auto &[var, reg]: allocatedVarRegs) {
        (void) var;
        if (reg >= Register::s0 && reg <= Register::s7) {
            registers.insert(reg);
        }
    }
    for (const auto &[temp, reg]: tempToRegs) {
        (void) temp;
        if (reg >= Register::s0 && reg <= Register::s7) {
            registers.insert(reg);
        }
    }
    return {registers.begin(), registers.end()};
}

bool MIPS::usesRegisterArguments(const IR::Function &function) {
    return registerArgumentFunctions.find(function.getName())
           != registerArgumentFunctions.end();
}

Register MIPS::argumentRegister(size_t index) {
    if (index >= 4) {
        Error::raise("MIPS argument register index out of range");
        return Register::a3;
    }
    return static_cast<Register>(static_cast<int>(Register::a0) + static_cast<int>(index));
}

void MIPS::emitFunctionPrologue(const IR::Function &function, bool isMain) {
    savedVariableRegs = usedVariableRegisters();
    restoreReturnAddress = !isMain && std::any_of(function.getBasicBlocks().begin(), function.getBasicBlocks().end(), [](const auto &block) {
        return std::any_of(block->instructions.begin(), block->instructions.end(), [](const IR::Inst &inst) {
            return inst.op == IR::Op::Call;
        });
    });

    if (!isMain) {
        if (restoreReturnAddress) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(Op::sw, Register::ra, Register::sp, -wordSize));
        }
        for (Register reg: savedVariableRegs) {
            assemblies.push_back(std::make_unique<I_imm_Inst>(
                    Op::sw, reg, Register::sp, -variableSaveOffset(reg)));
        }
    }

    int parameterOffset = 0;
    for (size_t index = 0; index < function.getParams().size(); ++index) {
        const auto &param = function.getParams()[index];
        IR::Var var(param.name, 1, false, param.dims, param.type, !param.dims.empty());
        auto allocation = allocatedVarRegs.find(var);
        if (allocation != allocatedVarRegs.end()) {
            varToRegs[var] = allocation->second;
            if (!usesRegisterArguments(function) || allocation->second != argumentRegister(index)) {
                assemblies.push_back(std::make_unique<I_imm_Inst>(
                        loadOp(param.type), allocation->second, Register::sp, parameterOffset));
            }
        }
        parameterOffset += wordSize;
    }
}

void MIPS::emitFunctionEpilogue() {
    for (auto iter = savedVariableRegs.rbegin(); iter != savedVariableRegs.rend(); ++iter) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::lw, *iter, Register::sp, -variableSaveOffset(*iter)));
    }
    if (restoreReturnAddress) {
        assemblies.push_back(std::make_unique<I_imm_Inst>(Op::lw, Register::ra, Register::sp, -wordSize));
    }
}

std::string MIPS::regToString(Register reg) {
    switch (reg) {
        case Register::zero:
            return "$zero";
        case Register::at:
            return "$at";
        case Register::v0:
            return "$v0";
        case Register::v1:
            return "$v1";
        case Register::a0:
            return "$a0";
        case Register::a1:
            return "$a1";
        case Register::a2:
            return "$a2";
        case Register::a3:
            return "$a3";
        case Register::t0:
            return "$t0";
        case Register::t1:
            return "$t1";
        case Register::t2:
            return "$t2";
        case Register::t3:
            return "$t3";
        case Register::t4:
            return "$t4";
        case Register::t5:
            return "$t5";
        case Register::t6:
            return "$t6";
        case Register::t7:
            return "$t7";
        case Register::s0:
            return "$s0";
        case Register::s1:
            return "$s1";
        case Register::s2:
            return "$s2";
        case Register::s3:
            return "$s3";
        case Register::s4:
            return "$s4";
        case Register::s5:
            return "$s5";
        case Register::s6:
            return "$s6";
        case Register::s7:
            return "$s7";
        case Register::t8:
            return "$t8";
        case Register::t9:
            return "$t9";
        case Register::k0:
            return "$k0";
        case Register::k1:
            return "$k1";
        case Register::gp:
            return "$gp";
        case Register::sp:
            return "$sp";
        case Register::fp:
            return "$fp";
        case Register::ra:
            return "$ra";
        case Register::none:
            return "";
        default:
            return "unknown";
    }
}

void MIPS::clearRegs() {
    tempToRegs.clear();
    varToRegs.clear();
    allocatedVarRegs.clear();
    spilledTemps.clear();
    rematerializedConstants.clear();
    coalescedLoadVariables.clear();
    coalescedStoreVariables.clear();
    liveTempsAfter.clear();
    instructionScratchRegs.clear();
    freeScratchRegs.clear();
    savedVariableRegs.clear();
    restoreReturnAddress = false;
}

void MIPS::checkTempReg(const IR::Temp *temp, Register reg) {
    if (temp->id >= 0 && tempToRegs.find(temp->id) == tempToRegs.end()) {
        if (rematerializedConstants.find(temp->id) != rematerializedConstants.end()) {
            return;
        }
        auto offset = StackMemory::tempToOffset.find(temp->id);
        if (offset == StackMemory::tempToOffset.end()) {
            Error::raise("Missing stack slot for spilled temporary");
            return;
        }
        assemblies.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, reg, Register::sp, -offset->second));
    }
}

bool MIPS::isRematerialized(const IR::Temp *temp) {
    return temp && temp->id >= 0 && tempToRegs.find(temp->id) == tempToRegs.end()
           && rematerializedConstants.find(temp->id) != rematerializedConstants.end();
}
