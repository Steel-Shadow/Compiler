#ifndef COMPILER_MIDDLE_ANALYSIS_H
#define COMPILER_MIDDLE_ANALYSIS_H

#include "middle/IRUtils.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IR {

struct ControlFlowGraph {
    static constexpr size_t NoBlock = static_cast<size_t>(-1);

    std::vector<std::vector<size_t>> successors;
    std::vector<std::vector<size_t>> predecessors;
    std::vector<std::unordered_set<size_t>> dominators;
    std::vector<size_t> immediateDominator;
    std::vector<std::vector<size_t>> dominatorTree;
    std::vector<std::vector<size_t>> dominanceFrontier;
    std::vector<size_t> dominatorDepth;
    std::vector<size_t> reversePostOrder;
    std::vector<bool> reachable;

    bool dominates(size_t dominator, size_t block) const;
};

using NaturalLoop = std::unordered_set<size_t>;
using NaturalLoops = std::unordered_map<size_t, NaturalLoop>;

struct TempLiveness {
    std::vector<TempSet> liveIn;
    std::vector<TempSet> liveOut;
    std::unordered_map<const Inst *, TempSet> liveAfter;
};

using VarSet = std::unordered_set<Var>;

struct VariableLiveness {
    std::vector<VarSet> liveIn;
    std::vector<VarSet> liveOut;
    std::unordered_map<const Inst *, VarSet> liveAfter;
    VarSet candidates;
};

ControlFlowGraph buildControlFlowGraph(const Function &function);
NaturalLoops collectNaturalLoops(const ControlFlowGraph &cfg);
std::vector<size_t> computeLoopDepths(const ControlFlowGraph &cfg);

// Checks the structural and dominance invariants expected by SSA-only passes.
bool verifySSA(const Function &function, std::string *reason = nullptr);

TempLiveness analyzeTempLiveness(const Function &function, const ControlFlowGraph &cfg);

bool isRegisterCandidate(const Var &var);
VariableLiveness analyzeVariableLiveness(const Function &function, const ControlFlowGraph &cfg);

} // namespace IR

#endif
