#ifndef COMPILER_MIPS_INTERNAL_H
#define COMPILER_MIPS_INTERNAL_H

#include "backend/MIPS.h"

#include <map>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MIPS::detail {

int alignTo(int value, int align);
int sizeOfIRType(const std::string &type);
std::string sanitizeLabel(std::string label);
std::string stripPrefix(std::string text);
bool isInteger(const std::string &text);
std::string edgeKey(const std::string &pred, const std::string &target);
std::string optimizeAssembly(const std::string &assembly);

struct Frame {
    std::unordered_map<std::string, int> valueSlots;
    std::unordered_map<std::string, int> pointerSlots;
    std::unordered_map<std::string, std::string> valueRegs;
    std::unordered_map<std::string, std::string> labels;
    std::vector<std::pair<std::string, int>> savedRegs;
    int nextOffset{0};
    int phiTempOffset{-1};
    int phiTempCount{0};
    int frameSize{0};
};

using ValueSet = std::set<std::string>;

struct BlockLiveness {
    ValueSet use;
    ValueSet def;
    ValueSet liveIn;
    ValueSet liveOut;
    std::vector<std::string> successors;
};

class FunctionEmitter {
public:
    FunctionEmitter(const IR::Function &function, std::ostream &out);

    void emit();

private:
    const IR::Function &function_;
    std::ostream &out_;
    Frame frame_;
    std::unordered_map<std::string, std::vector<std::pair<IR::Operand, std::string>>> phiMoves_;
    std::unordered_map<std::string, int> useCounts_;
    std::set<std::string> neededValues_;
    std::set<std::string> callLiveValues_;
    std::string currentBlock_;
    int edgeId_{0};
    inline static const std::vector<std::string> callerSavedRegs_ = {
            "$t3", "$t4", "$t5", "$t6", "$t7",
    };
    inline static const std::vector<std::string> calleeSavedRegs_ = {
            "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    };
    inline static const std::vector<std::string> allocatableRegs_ = {
            "$t3", "$t4", "$t5", "$t6", "$t7",
            "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    };

    int reserveSlot(int bytes = 4);
    void buildFrame();
    std::vector<std::string> usedAllocatedRegs() const;

    void allocateRegisters();
    ValueSet collectRegisterCandidates() const;
    static bool isCandidateValue(const IR::Operand &operand, const ValueSet &values);
    static void addOperandUse(const IR::Operand &operand, const ValueSet &values, ValueSet &uses);
    ValueSet collectNeededValues() const;
    std::vector<std::string> successorsOf(const IR::BasicBlock &block) const;
    std::map<std::string, ValueSet> collectPhiDefs(const ValueSet &values) const;
    std::map<std::string, ValueSet> collectPhiEdgeUses(const ValueSet &values) const;
    std::map<std::string, BlockLiveness> buildLiveness(const ValueSet &values) const;
    ValueSet collectCallLiveValues(const ValueSet &values, const std::map<std::string, BlockLiveness> &liveness) const;
    static void addInterference(std::map<std::string, ValueSet> &graph,
                                const std::string &lhs,
                                const std::string &rhs);
    std::map<std::string, ValueSet> buildInterferenceGraph(const ValueSet &values,
                                                           const std::map<std::string, BlockLiveness> &liveness) const;
    const std::vector<std::string> &allowedRegistersFor(const std::string &value) const;
    void colorInterferenceGraph(const ValueSet &values, const std::map<std::string, ValueSet> &graph);

    void collectUseCounts();
    void collectPhiMoves();
    void spillParameters();
    std::string labelOf(const std::string &blockName) const;
    void emitInst(const IR::Instruction &inst);
    bool canFuseICmpBranch(const std::vector<IR::Instruction> &instructions, size_t index) const;
    bool canFusePowerOfTwoRemainderBranch(const std::vector<IR::Instruction> &instructions, size_t index) const;
    std::string labelFromOperand(const IR::Operand &operand) const;
    static std::string labelName(const IR::Operand &operand);

    bool hasPhiMoves(const std::string &target) const;
    std::string assignedRegister(const std::string &name) const;
    std::string resultRegister(const std::string &name, const std::string &fallback) const;
    std::string valueLocation(const std::string &name) const;
    std::string operandLocation(const IR::Operand &operand) const;
    bool isPhysicalSelfMove(const IR::Operand &value, const std::string &result) const;
    bool needsParallelCopy(const std::vector<std::pair<IR::Operand, std::string>> &moves) const;
    void emitDirectPhiMove(const IR::Operand &value, const std::string &result);
    void emitPhiMoves(const std::string &target);
    void emitCondBranchWithPhi(const IR::Instruction &inst, const std::string &condReg = "$t0");

    std::string materializeOperand(const IR::Operand &operand, const std::string &scratch);
    std::string materializePointerAddress(const IR::Operand &operand, const std::string &scratch);
    void loadOperand(const IR::Operand &operand, const std::string &reg);
    void loadPointerAddress(const IR::Operand &operand, const std::string &reg);
    void storeValue(const std::string &name, const std::string &reg);

    void emitLoad(const IR::Instruction &inst);
    void emitStore(const IR::Instruction &inst);
    void emitGetElementPtr(const IR::Instruction &inst);
    void emitBinary(const IR::Instruction &inst);
    void emitICmp(const IR::Instruction &inst);
    void emitICmpToReg(const IR::Instruction &inst, const std::string &dest);
    void emitPowerOfTwoRemainderBranch(const IR::Instruction &rem,
                                       const IR::Instruction &cmp,
                                       const IR::Instruction &branch);
    void emitCast(const IR::Instruction &inst);
    void emitCall(const IR::Instruction &inst);
    void emitReturn(const IR::Instruction &inst);
    void emitDefaultReturn();
    void restoreSavedRegs();
};

} // namespace MIPS::detail

#endif
