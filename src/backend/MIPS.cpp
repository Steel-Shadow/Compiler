#include "backend/MIPS.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MIPS {
namespace {

int alignTo(int value, int align) {
    return ((value + align - 1) / align) * align;
}

int sizeOfIRType(const std::string &type) {
    return type == "i8" ? 1 : 4;
}

std::string sanitizeLabel(std::string label) {
    for (char &ch: label) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            ch = '_';
        }
    }
    return label;
}

std::string stripPrefix(std::string text) {
    if (!text.empty() && (text.front() == '%' || text.front() == '@')) {
        text.erase(text.begin());
    }
    return text;
}

bool isInteger(const std::string &text) {
    if (text.empty()) {
        return false;
    }
    size_t pos = text[0] == '-' ? 1 : 0;
    if (pos == text.size()) {
        return false;
    }
    for (; pos < text.size(); ++pos) {
        if (!std::isdigit(static_cast<unsigned char>(text[pos]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool startsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool parseMemInst(const std::string &line, const std::string &op, std::string &reg, std::string &addr) {
    std::string prefix = "  " + op + " ";
    if (!startsWith(line, prefix)) {
        return false;
    }
    size_t comma = line.find(", ", prefix.size());
    if (comma == std::string::npos) {
        return false;
    }
    reg = line.substr(prefix.size(), comma - prefix.size());
    addr = line.substr(comma + 2);
    return true;
}

std::string optimizeAssembly(const std::string &assembly) {
    auto lines = splitLines(assembly);
    std::vector<std::string> optimized;
    optimized.reserve(lines.size());

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (startsWith(line, "  li ") && line.size() > 6 && line.rfind(", 0") == line.size() - 3) {
            std::string reg = line.substr(5, line.size() - 8);
            line = "  move " + reg + ", $zero";
        }

        if (startsWith(line, "  j ") && i + 1 < lines.size()) {
            std::string target = line.substr(4);
            if (lines[i + 1] == target + ":") {
                continue;
            }
        }

        if (!optimized.empty()) {
            std::string swReg;
            std::string swAddr;
            std::string lwReg;
            std::string lwAddr;
            if (parseMemInst(optimized.back(), "sw", swReg, swAddr) && parseMemInst(line, "lw", lwReg, lwAddr) && swAddr == lwAddr) {
                if (swReg == lwReg) {
                    continue;
                }
                line = "  move " + lwReg + ", " + swReg;
            }
        }

        optimized.push_back(std::move(line));
    }

    std::ostringstream out;
    for (const auto &line: optimized) {
        out << line << "\n";
    }
    return out.str();
}

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

std::string edgeKey(const std::string &pred, const std::string &target) {
    return pred + "\n" + target;
}

class FunctionEmitter {
public:
    FunctionEmitter(const IR::Function &function, std::ostream &out) :
        function_(function), out_(out) {}

    void emit() {
        buildFrame();
        out_ << "\n" << function_.name << ":\n";
        out_ << "  addiu $sp, $sp, -" << frame_.frameSize << "\n";
        out_ << "  sw $ra, " << frame_.frameSize - 4 << "($sp)\n";
        for (const auto &[reg, offset]: frame_.savedRegs) {
            out_ << "  sw " << reg << ", " << offset << "($sp)\n";
        }
        spillParameters();
        for (const auto &block: function_.blocks) {
            currentBlock_ = block->name;
            out_ << labelOf(block->name) << ":\n";
            for (size_t i = 0; i < block->instructions.size(); ++i) {
                if (canFusePowerOfTwoRemainderBranch(block->instructions, i)) {
                    emitPowerOfTwoRemainderBranch(block->instructions[i], block->instructions[i + 1], block->instructions[i + 2]);
                    i += 2;
                    continue;
                }
                if (canFuseICmpBranch(block->instructions, i)) {
                    emitICmpToReg(block->instructions[i], "$t0");
                    emitCondBranchWithPhi(block->instructions[i + 1]);
                    ++i;
                    continue;
                }
                emitInst(block->instructions[i]);
            }
        }
        if (function_.blocks.empty() || !function_.blocks.back()->terminated()) {
            emitDefaultReturn();
        }
    }

private:
    const IR::Function &function_;
    std::ostream &out_;
    Frame frame_;
    std::unordered_map<std::string, std::vector<std::pair<IR::Operand, std::string>>> phiMoves_;
    std::unordered_map<std::string, int> useCounts_;
    std::set<std::string> neededValues_;
    std::string currentBlock_;
    int edgeId_{0};
    inline static const std::vector<std::string> allocatableRegs_ = {
            "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    };

    int reserveSlot(int bytes = 4) {
        frame_.nextOffset = alignTo(frame_.nextOffset, 4);
        int offset = frame_.nextOffset;
        frame_.nextOffset += alignTo(bytes, 4);
        return offset;
    }

    void buildFrame() {
        collectUseCounts();
        neededValues_ = collectNeededValues();
        allocateRegisters();
        for (const auto &reg: usedAllocatedRegs()) {
            frame_.savedRegs.push_back({reg, reserveSlot()});
        }
        for (const auto &param: function_.params) {
            auto name = "%" + param.name;
            if (frame_.valueRegs.find(name) == frame_.valueRegs.end()) {
                frame_.valueSlots[name] = reserveSlot();
            }
        }
        for (const auto &block: function_.blocks) {
            frame_.labels[block->name] = sanitizeLabel(function_.name + "_" + block->name);
            for (const auto &inst: block->instructions) {
                if (inst.opcode == IR::Opcode::Alloca) {
                    int count = 1;
                    if (!inst.operands.empty() && isInteger(inst.operands.front().text)) {
                        count = std::max(1, std::stoi(inst.operands.front().text));
                    }
                    frame_.pointerSlots[inst.result] = reserveSlot(count * sizeOfIRType(inst.type));
                } else if (inst.hasResult()) {
                    if (frame_.valueRegs.find(inst.result) == frame_.valueRegs.end()) {
                        frame_.valueSlots[inst.result] = reserveSlot();
                    }
                }
            }
        }
        collectPhiMoves();
        for (const auto &[edge, moves]: phiMoves_) {
            (void) edge;
            frame_.phiTempCount = std::max(frame_.phiTempCount, static_cast<int>(moves.size()));
        }
        if (frame_.phiTempCount > 0) {
            frame_.phiTempOffset = reserveSlot(frame_.phiTempCount * 4);
        }
        frame_.frameSize = alignTo(frame_.nextOffset + 8, 8);
    }

    std::vector<std::string> usedAllocatedRegs() const {
        std::set<std::string> used;
        for (const auto &[value, reg]: frame_.valueRegs) {
            (void) value;
            used.insert(reg);
        }
        std::vector<std::string> ordered;
        for (const auto &reg: allocatableRegs_) {
            if (used.find(reg) != used.end()) {
                ordered.push_back(reg);
            }
        }
        return ordered;
    }

    using ValueSet = std::set<std::string>;

    struct BlockLiveness {
        ValueSet use;
        ValueSet def;
        ValueSet liveIn;
        ValueSet liveOut;
        std::vector<std::string> successors;
    };

    void allocateRegisters() {
        ValueSet values = collectRegisterCandidates();
        if (values.empty()) {
            return;
        }

        std::map<std::string, BlockLiveness> liveness = buildLiveness(values);
        std::map<std::string, ValueSet> graph = buildInterferenceGraph(values, liveness);
        colorInterferenceGraph(values, graph);
    }

    ValueSet collectRegisterCandidates() const {
        ValueSet values;
        for (const auto &param: function_.params) {
            values.insert("%" + param.name);
        }
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode == IR::Opcode::Phi && neededValues_.find(inst.result) == neededValues_.end()) {
                    continue;
                }
                if (inst.opcode != IR::Opcode::Alloca && inst.hasResult()) {
                    values.insert(inst.result);
                }
            }
        }
        return values;
    }

    static bool isCandidateValue(const IR::Operand &operand, const ValueSet &values) {
        return values.find(operand.text) != values.end();
    }

    static void addOperandUse(const IR::Operand &operand, const ValueSet &values, ValueSet &uses) {
        if (isCandidateValue(operand, values)) {
            uses.insert(operand.text);
        }
    }

    ValueSet collectNeededValues() const {
        ValueSet needed;
        auto mark = [&needed](const IR::Operand &operand) {
            if (!operand.text.empty() && operand.text.front() == '%' && operand.type != "label") {
                needed.insert(operand.text);
            }
        };

        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode == IR::Opcode::Phi) {
                    continue;
                }
                for (const auto &operand: inst.operands) {
                    mark(operand);
                }
            }
        }

        bool changed;
        do {
            changed = false;
            for (const auto &block: function_.blocks) {
                for (const auto &inst: block->instructions) {
                    if (inst.opcode != IR::Opcode::Phi || needed.find(inst.result) == needed.end()) {
                        continue;
                    }
                    for (const auto &incoming: inst.incoming) {
                        if (!incoming.value.text.empty() && incoming.value.text.front() == '%' &&
                            needed.insert(incoming.value.text).second) {
                            changed = true;
                        }
                    }
                }
            }
        } while (changed);
        return needed;
    }

    std::vector<std::string> successorsOf(const IR::BasicBlock &block) const {
        std::vector<std::string> successors;
        if (block.instructions.empty()) {
            return successors;
        }
        const auto &term = block.instructions.back();
        if (term.opcode == IR::Opcode::Br) {
            successors.push_back(labelName(term.operands.front()));
        } else if (term.opcode == IR::Opcode::CondBr) {
            successors.push_back(labelName(term.operands[1]));
            successors.push_back(labelName(term.operands[2]));
        }
        return successors;
    }

    std::map<std::string, ValueSet> collectPhiDefs(const ValueSet &values) const {
        std::map<std::string, ValueSet> phiDefs;
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode == IR::Opcode::Phi && values.find(inst.result) != values.end()) {
                    phiDefs[block->name].insert(inst.result);
                }
            }
        }
        return phiDefs;
    }

    std::map<std::string, ValueSet> collectPhiEdgeUses(const ValueSet &values) const {
        std::map<std::string, ValueSet> edgeUses;
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode != IR::Opcode::Phi) {
                    continue;
                }
                for (const auto &incoming: inst.incoming) {
                    if (isCandidateValue(incoming.value, values)) {
                        edgeUses[edgeKey(incoming.block, block->name)].insert(incoming.value.text);
                    }
                }
            }
        }
        return edgeUses;
    }

    std::map<std::string, BlockLiveness> buildLiveness(const ValueSet &values) const {
        std::map<std::string, BlockLiveness> blocks;
        for (const auto &blockPtr: function_.blocks) {
            const auto &block = *blockPtr;
            auto &info = blocks[block.name];
            info.successors = successorsOf(block);
            for (const auto &inst: block.instructions) {
                if (inst.opcode != IR::Opcode::Phi) {
                    for (const auto &operand: inst.operands) {
                        if (isCandidateValue(operand, values) && info.def.find(operand.text) == info.def.end()) {
                            info.use.insert(operand.text);
                        }
                    }
                }
                if (inst.opcode != IR::Opcode::Alloca && inst.hasResult() && values.find(inst.result) != values.end()) {
                    info.def.insert(inst.result);
                }
            }
        }

        auto phiDefs = collectPhiDefs(values);
        auto phiEdgeUses = collectPhiEdgeUses(values);
        bool changed;
        do {
            changed = false;
            for (auto it = function_.blocks.rbegin(); it != function_.blocks.rend(); ++it) {
                const auto &block = **it;
                auto &info = blocks[block.name];
                ValueSet liveOut;
                for (const auto &succ: info.successors) {
                    ValueSet succIn = blocks[succ].liveIn;
                    auto defIt = phiDefs.find(succ);
                    if (defIt != phiDefs.end()) {
                        for (const auto &phiDef: defIt->second) {
                            succIn.erase(phiDef);
                        }
                    }
                    liveOut.insert(succIn.begin(), succIn.end());
                    auto edgeIt = phiEdgeUses.find(edgeKey(block.name, succ));
                    if (edgeIt != phiEdgeUses.end()) {
                        liveOut.insert(edgeIt->second.begin(), edgeIt->second.end());
                    }
                }

                ValueSet liveIn = info.use;
                for (const auto &value: liveOut) {
                    if (info.def.find(value) == info.def.end()) {
                        liveIn.insert(value);
                    }
                }

                if (liveIn != info.liveIn || liveOut != info.liveOut) {
                    info.liveIn = std::move(liveIn);
                    info.liveOut = std::move(liveOut);
                    changed = true;
                }
            }
        } while (changed);
        return blocks;
    }

    static void addInterference(std::map<std::string, ValueSet> &graph,
                                const std::string &lhs,
                                const std::string &rhs) {
        if (lhs == rhs) {
            return;
        }
        graph[lhs].insert(rhs);
        graph[rhs].insert(lhs);
    }

    std::map<std::string, ValueSet> buildInterferenceGraph(const ValueSet &values,
                                                           const std::map<std::string, BlockLiveness> &liveness) const {
        std::map<std::string, ValueSet> graph;
        for (const auto &value: values) {
            graph[value];
        }
        for (size_t i = 0; i < function_.params.size(); ++i) {
            std::string lhs = "%" + function_.params[i].name;
            if (values.find(lhs) == values.end()) {
                continue;
            }
            for (size_t j = i + 1; j < function_.params.size(); ++j) {
                std::string rhs = "%" + function_.params[j].name;
                if (values.find(rhs) != values.end()) {
                    addInterference(graph, lhs, rhs);
                }
            }
        }

        for (const auto &blockPtr: function_.blocks) {
            const auto &block = *blockPtr;
            ValueSet live = liveness.at(block.name).liveOut;
            for (auto instIt = block.instructions.rbegin(); instIt != block.instructions.rend(); ++instIt) {
                const auto &inst = *instIt;
                bool hasDef = inst.opcode != IR::Opcode::Alloca && inst.hasResult() && values.find(inst.result) != values.end();
                if (hasDef) {
                    for (const auto &liveValue: live) {
                        addInterference(graph, inst.result, liveValue);
                    }
                    live.erase(inst.result);
                }
                if (inst.opcode != IR::Opcode::Phi) {
                    for (const auto &operand: inst.operands) {
                        addOperandUse(operand, values, live);
                    }
                }
            }
        }
        return graph;
    }

    void colorInterferenceGraph(const ValueSet &values, const std::map<std::string, ValueSet> &graph) {
        const int k = static_cast<int>(allocatableRegs_.size());
        std::map<std::string, ValueSet> workGraph = graph;
        std::vector<std::string> stack;
        stack.reserve(values.size());

        while (!workGraph.empty()) {
            auto chosen = workGraph.end();
            for (auto it = workGraph.begin(); it != workGraph.end(); ++it) {
                if (static_cast<int>(it->second.size()) < k) {
                    chosen = it;
                    break;
                }
            }
            if (chosen == workGraph.end()) {
                chosen = std::max_element(workGraph.begin(), workGraph.end(), [](const auto &lhs, const auto &rhs) {
                    if (lhs.second.size() != rhs.second.size()) {
                        return lhs.second.size() < rhs.second.size();
                    }
                    return lhs.first < rhs.first;
                });
            }

            std::string node = chosen->first;
            stack.push_back(node);
            for (const auto &neighbor: chosen->second) {
                auto neighborIt = workGraph.find(neighbor);
                if (neighborIt != workGraph.end()) {
                    neighborIt->second.erase(node);
                }
            }
            workGraph.erase(chosen);
        }

        std::map<std::string, std::string> colors;
        while (!stack.empty()) {
            std::string node = stack.back();
            stack.pop_back();
            std::set<std::string> unavailable;
            auto graphIt = graph.find(node);
            if (graphIt != graph.end()) {
                for (const auto &neighbor: graphIt->second) {
                    auto colorIt = colors.find(neighbor);
                    if (colorIt != colors.end()) {
                        unavailable.insert(colorIt->second);
                    }
                }
            }
            for (const auto &reg: allocatableRegs_) {
                if (unavailable.find(reg) == unavailable.end()) {
                    colors[node] = reg;
                    frame_.valueRegs[node] = reg;
                    break;
                }
            }
        }
    }

    void collectUseCounts() {
        useCounts_.clear();
        auto count = [this](const IR::Operand &operand) {
            if (!operand.text.empty() && operand.text.front() == '%') {
                ++useCounts_[operand.text];
            }
        };
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                for (const auto &operand: inst.operands) {
                    count(operand);
                }
                for (const auto &incoming: inst.incoming) {
                    count(incoming.value);
                }
            }
        }
    }

    void collectPhiMoves() {
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode != IR::Opcode::Phi) {
                    continue;
                }
                if (neededValues_.find(inst.result) == neededValues_.end()) {
                    continue;
                }
                for (const auto &incoming: inst.incoming) {
                    phiMoves_[edgeKey(incoming.block, block->name)].push_back({incoming.value, inst.result});
                }
            }
        }
        for (auto &[edge, moves]: phiMoves_) {
            (void) edge;
            moves.erase(std::remove_if(moves.begin(), moves.end(), [](const auto &move) {
                return move.first.text == move.second;
            }), moves.end());
        }
    }

    void spillParameters() {
        static const char *argRegs[] = {"$a0", "$a1", "$a2", "$a3"};
        for (size_t i = 0; i < function_.params.size() && i < 4; ++i) {
            auto name = "%" + function_.params[i].name;
            auto regIt = frame_.valueRegs.find(name);
            if (regIt != frame_.valueRegs.end()) {
                out_ << "  move " << regIt->second << ", " << argRegs[i] << "\n";
            } else {
                out_ << "  sw " << argRegs[i] << ", " << frame_.valueSlots[name] << "($sp)\n";
            }
        }
        for (size_t i = 4; i < function_.params.size(); ++i) {
            auto name = "%" + function_.params[i].name;
            int callerArgOffset = frame_.frameSize + static_cast<int>((i - 4) * 4);
            auto regIt = frame_.valueRegs.find(name);
            if (regIt != frame_.valueRegs.end()) {
                out_ << "  lw " << regIt->second << ", " << callerArgOffset << "($sp)\n";
            } else {
                out_ << "  lw $t0, " << callerArgOffset << "($sp)\n";
                out_ << "  sw $t0, " << frame_.valueSlots[name] << "($sp)\n";
            }
        }
    }

    std::string labelOf(const std::string &blockName) const {
        auto it = frame_.labels.find(blockName);
        return it == frame_.labels.end() ? sanitizeLabel(function_.name + "_" + blockName) : it->second;
    }

    void emitInst(const IR::Instruction &inst) {
        switch (inst.opcode) {
            case IR::Opcode::Alloca:
                break;
            case IR::Opcode::Load:
                emitLoad(inst);
                break;
            case IR::Opcode::Store:
                emitStore(inst);
                break;
            case IR::Opcode::Binary:
                emitBinary(inst);
                break;
            case IR::Opcode::ICmp:
                emitICmp(inst);
                break;
            case IR::Opcode::Br:
                emitPhiMoves(labelName(inst.operands.front()));
                out_ << "  j " << labelFromOperand(inst.operands.front()) << "\n";
                break;
            case IR::Opcode::CondBr:
                loadOperand(inst.operands[0], "$t0");
                emitCondBranchWithPhi(inst);
                break;
            case IR::Opcode::Ret:
                emitReturn(inst);
                break;
            case IR::Opcode::Call:
                emitCall(inst);
                break;
            case IR::Opcode::Phi:
                break;
            case IR::Opcode::GetElementPtr:
                emitGetElementPtr(inst);
                break;
            case IR::Opcode::Cast:
                emitCast(inst);
                break;
            case IR::Opcode::Comment:
                out_ << "  # " << inst.note << "\n";
                break;
        }
    }

    bool canFuseICmpBranch(const std::vector<IR::Instruction> &instructions, size_t index) const {
        if (index + 1 >= instructions.size()) {
            return false;
        }
        const auto &cmp = instructions[index];
        const auto &branch = instructions[index + 1];
        if (cmp.opcode != IR::Opcode::ICmp || branch.opcode != IR::Opcode::CondBr || branch.operands.empty()) {
            return false;
        }
        if (branch.operands[0].text != cmp.result) {
            return false;
        }
        auto it = useCounts_.find(cmp.result);
        return it != useCounts_.end() && it->second == 1;
    }

    bool canFusePowerOfTwoRemainderBranch(const std::vector<IR::Instruction> &instructions, size_t index) const {
        if (index + 2 >= instructions.size()) {
            return false;
        }
        const auto &rem = instructions[index];
        const auto &cmp = instructions[index + 1];
        const auto &branch = instructions[index + 2];
        if (rem.opcode != IR::Opcode::Binary || rem.op != "srem" || cmp.opcode != IR::Opcode::ICmp ||
            branch.opcode != IR::Opcode::CondBr) {
            return false;
        }
        if (cmp.op != "eq" && cmp.op != "ne") {
            return false;
        }
        if (branch.operands.empty() || branch.operands[0].text != cmp.result) {
            return false;
        }
        if (useCounts_.find(rem.result) == useCounts_.end() || useCounts_.at(rem.result) != 1 ||
            useCounts_.find(cmp.result) == useCounts_.end() || useCounts_.at(cmp.result) != 1) {
            return false;
        }
        if (rem.operands.size() < 2 || !isInteger(rem.operands[1].text)) {
            return false;
        }
        int divisor = std::stoi(rem.operands[1].text);
        if (divisor <= 0 || (divisor & (divisor - 1)) != 0) {
            return false;
        }
        bool remIsLhs = cmp.operands[0].text == rem.result && isInteger(cmp.operands[1].text) && std::stoi(cmp.operands[1].text) == 0;
        bool remIsRhs = cmp.operands[1].text == rem.result && isInteger(cmp.operands[0].text) && std::stoi(cmp.operands[0].text) == 0;
        return remIsLhs || remIsRhs;
    }

    std::string labelFromOperand(const IR::Operand &operand) const {
        return labelOf(labelName(operand));
    }

    static std::string labelName(const IR::Operand &operand) {
        std::string label = operand.text;
        if (!label.empty() && label.front() == '%') {
            label.erase(label.begin());
        }
        return label;
    }

    bool hasPhiMoves(const std::string &target) const {
        auto it = phiMoves_.find(edgeKey(currentBlock_, target));
        return it != phiMoves_.end() && !it->second.empty();
    }

    std::string valueLocation(const std::string &name) const {
        auto regIt = frame_.valueRegs.find(name);
        if (regIt != frame_.valueRegs.end()) {
            return "reg:" + regIt->second;
        }
        auto slotIt = frame_.valueSlots.find(name);
        if (slotIt != frame_.valueSlots.end()) {
            return "slot:" + std::to_string(slotIt->second);
        }
        return "";
    }

    std::string operandLocation(const IR::Operand &operand) const {
        if (operand.text.empty() || operand.text.front() != '%') {
            return "";
        }
        return valueLocation(operand.text);
    }

    bool isPhysicalSelfMove(const IR::Operand &value, const std::string &result) const {
        std::string source = operandLocation(value);
        std::string dest = valueLocation(result);
        return !source.empty() && source == dest;
    }

    bool needsParallelCopy(const std::vector<std::pair<IR::Operand, std::string>> &moves) const {
        std::unordered_set<std::string> destinations;
        for (const auto &[value, result]: moves) {
            (void) value;
            std::string dest = valueLocation(result);
            if (!dest.empty()) {
                destinations.insert(dest);
            }
        }
        for (const auto &[value, result]: moves) {
            std::string source = operandLocation(value);
            std::string dest = valueLocation(result);
            if (!source.empty() && source != dest && destinations.find(source) != destinations.end()) {
                return true;
            }
        }
        return false;
    }

    void emitPhiMoves(const std::string &target) {
        auto it = phiMoves_.find(edgeKey(currentBlock_, target));
        if (it == phiMoves_.end()) {
            return;
        }
        if (!needsParallelCopy(it->second)) {
            for (const auto &[value, result]: it->second) {
                if (isPhysicalSelfMove(value, result)) {
                    continue;
                }
                loadOperand(value, "$t8");
                storeValue(result, "$t8");
            }
            return;
        }
        for (size_t i = 0; i < it->second.size(); ++i) {
            const auto &[value, result] = it->second[i];
            (void) result;
            if (isPhysicalSelfMove(value, result)) {
                out_ << "  move $t8, $zero\n";
                out_ << "  sw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
                continue;
            }
            loadOperand(value, "$t8");
            out_ << "  sw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
        }
        for (size_t i = 0; i < it->second.size(); ++i) {
            const auto &[value, result] = it->second[i];
            (void) value;
            if (isPhysicalSelfMove(value, result)) {
                continue;
            }
            out_ << "  lw $t8, " << frame_.phiTempOffset + static_cast<int>(i * 4) << "($sp)\n";
            storeValue(result, "$t8");
        }
    }

    void emitCondBranchWithPhi(const IR::Instruction &inst) {
        std::string trueTarget = labelName(inst.operands[1]);
        std::string falseTarget = labelName(inst.operands[2]);
        if (!hasPhiMoves(trueTarget) && !hasPhiMoves(falseTarget)) {
            out_ << "  bne $t0, $zero, " << labelOf(trueTarget) << "\n";
            out_ << "  j " << labelOf(falseTarget) << "\n";
            return;
        }
        std::string trueEdge = sanitizeLabel(function_.name + "_" + currentBlock_ + "_to_" + trueTarget + "_" + std::to_string(edgeId_++));
        out_ << "  bne $t0, $zero, " << trueEdge << "\n";
        emitPhiMoves(falseTarget);
        out_ << "  j " << labelOf(falseTarget) << "\n";
        out_ << trueEdge << ":\n";
        emitPhiMoves(trueTarget);
        out_ << "  j " << labelOf(trueTarget) << "\n";
    }

    void loadOperand(const IR::Operand &operand, const std::string &reg) {
        if (operand.text.empty()) {
            out_ << "  move " << reg << ", $zero\n";
        } else if (operand.type == "ptr") {
            loadPointerAddress(operand, reg);
        } else if (isInteger(operand.text)) {
            out_ << "  li " << reg << ", " << operand.text << "\n";
        } else if (operand.text.front() == '@') {
            const std::string global = stripPrefix(operand.text);
            out_ << "  " << (operand.type == "i8" ? "lbu" : "lw") << " " << reg << ", " << global << "\n";
        } else {
            auto regIt = frame_.valueRegs.find(operand.text);
            if (regIt != frame_.valueRegs.end()) {
                if (regIt->second != reg) {
                    out_ << "  move " << reg << ", " << regIt->second << "\n";
                }
                return;
            }
            auto it = frame_.valueSlots.find(operand.text);
            if (it == frame_.valueSlots.end()) {
                out_ << "  # unknown operand " << operand.text << "\n";
                out_ << "  move " << reg << ", $zero\n";
            } else {
                out_ << "  lw " << reg << ", " << it->second << "($sp)\n";
            }
        }
    }

    void loadPointerAddress(const IR::Operand &operand, const std::string &reg) {
        if (operand.text.empty()) {
            out_ << "  move " << reg << ", $zero\n";
            return;
        }
        if (operand.text.front() == '@') {
            out_ << "  la " << reg << ", " << stripPrefix(operand.text) << "\n";
            return;
        }
        auto regIt = frame_.valueRegs.find(operand.text);
        if (regIt != frame_.valueRegs.end()) {
            if (regIt->second != reg) {
                out_ << "  move " << reg << ", " << regIt->second << "\n";
            }
            return;
        }
        auto ptrSlot = frame_.pointerSlots.find(operand.text);
        if (ptrSlot != frame_.pointerSlots.end()) {
            out_ << "  addiu " << reg << ", $sp, " << ptrSlot->second << "\n";
            return;
        }
        auto valueSlot = frame_.valueSlots.find(operand.text);
        if (valueSlot != frame_.valueSlots.end()) {
            out_ << "  lw " << reg << ", " << valueSlot->second << "($sp)\n";
            return;
        }
        out_ << "  # unknown pointer " << operand.text << "\n";
        out_ << "  move " << reg << ", $zero\n";
    }

    void storeValue(const std::string &name, const std::string &reg) {
        auto regIt = frame_.valueRegs.find(name);
        if (regIt != frame_.valueRegs.end()) {
            if (regIt->second != reg) {
                out_ << "  move " << regIt->second << ", " << reg << "\n";
            }
            return;
        }
        auto it = frame_.valueSlots.find(name);
        if (it != frame_.valueSlots.end()) {
            out_ << "  sw " << reg << ", " << it->second << "($sp)\n";
        }
    }

    void emitLoad(const IR::Instruction &inst) {
        const auto &ptr = inst.operands.front();
        if (!ptr.text.empty() && ptr.text.front() == '@') {
            out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " $t0, " << stripPrefix(ptr.text) << "\n";
        } else if (frame_.pointerSlots.find(ptr.text) != frame_.pointerSlots.end()) {
            auto it = frame_.pointerSlots.find(ptr.text);
            out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " $t0, " << it->second << "($sp)\n";
        } else {
            loadPointerAddress(ptr, "$t9");
            out_ << "  " << (inst.type == "i8" ? "lbu" : "lw") << " $t0, 0($t9)\n";
        }
        storeValue(inst.result, "$t0");
    }

    void emitStore(const IR::Instruction &inst) {
        loadOperand(inst.operands[0], "$t0");
        const auto &ptr = inst.operands[1];
        if (!ptr.text.empty() && ptr.text.front() == '@') {
            out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " $t0, " << stripPrefix(ptr.text) << "\n";
        } else if (frame_.pointerSlots.find(ptr.text) != frame_.pointerSlots.end()) {
            auto it = frame_.pointerSlots.find(ptr.text);
            out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " $t0, " << it->second << "($sp)\n";
        } else {
            loadPointerAddress(ptr, "$t9");
            out_ << "  " << (inst.operands[0].type == "i8" ? "sb" : "sw") << " $t0, 0($t9)\n";
        }
    }

    void emitGetElementPtr(const IR::Instruction &inst) {
        loadPointerAddress(inst.operands[0], "$t0");
        loadOperand(inst.operands[1], "$t1");
        if (inst.note == "i32") {
            out_ << "  sll $t1, $t1, 2\n";
        }
        out_ << "  addu $t2, $t0, $t1\n";
        storeValue(inst.result, "$t2");
    }

    void emitBinary(const IR::Instruction &inst) {
        loadOperand(inst.operands[0], "$t0");
        loadOperand(inst.operands[1], "$t1");
        if (inst.op == "add") {
            out_ << "  addu $t2, $t0, $t1\n";
        } else if (inst.op == "sub") {
            out_ << "  subu $t2, $t0, $t1\n";
        } else if (inst.op == "mul") {
            out_ << "  mul $t2, $t0, $t1\n";
        } else if (inst.op == "sdiv") {
            out_ << "  div $t0, $t1\n";
            out_ << "  mflo $t2\n";
        } else if (inst.op == "srem") {
            out_ << "  div $t0, $t1\n";
            out_ << "  mfhi $t2\n";
        } else if (inst.op == "and") {
            out_ << "  and $t2, $t0, $t1\n";
        } else if (inst.op == "or") {
            out_ << "  or $t2, $t0, $t1\n";
        } else {
            out_ << "  # unsupported binary op " << inst.op << "\n";
            out_ << "  move $t2, $zero\n";
        }
        storeValue(inst.result, "$t2");
    }

    void emitICmp(const IR::Instruction &inst) {
        emitICmpToReg(inst, "$t2");
        storeValue(inst.result, "$t2");
    }

    void emitICmpToReg(const IR::Instruction &inst, const std::string &dest) {
        loadOperand(inst.operands[0], "$t0");
        loadOperand(inst.operands[1], "$t1");
        if (inst.op == "slt") {
            out_ << "  slt $t2, $t0, $t1\n";
        } else if (inst.op == "sgt") {
            out_ << "  slt $t2, $t1, $t0\n";
        } else if (inst.op == "sle") {
            out_ << "  slt $t2, $t1, $t0\n";
            out_ << "  xori $t2, $t2, 1\n";
        } else if (inst.op == "sge") {
            out_ << "  slt $t2, $t0, $t1\n";
            out_ << "  xori $t2, $t2, 1\n";
        } else if (inst.op == "eq") {
            out_ << "  seq $t2, $t0, $t1\n";
        } else if (inst.op == "ne") {
            out_ << "  sne $t2, $t0, $t1\n";
        } else {
            out_ << "  # unsupported icmp " << inst.op << "\n";
            out_ << "  move $t2, $zero\n";
        }
        if (dest != "$t2") {
            out_ << "  move " << dest << ", $t2\n";
        }
    }

    void emitPowerOfTwoRemainderBranch(const IR::Instruction &rem,
                                       const IR::Instruction &cmp,
                                       const IR::Instruction &branch) {
        int divisor = std::stoi(rem.operands[1].text);
        loadOperand(rem.operands[0], "$t0");
        out_ << "  andi $t2, $t0, " << (divisor - 1) << "\n";
        if (cmp.op == "eq") {
            out_ << "  seq $t0, $t2, $zero\n";
        } else {
            out_ << "  sne $t0, $t2, $zero\n";
        }
        emitCondBranchWithPhi(branch);
    }

    void emitCast(const IR::Instruction &inst) {
        loadOperand(inst.operands.front(), "$t0");
        if (inst.op == "trunc") {
            out_ << "  andi $t0, $t0, 255\n";
        }
        storeValue(inst.result, "$t0");
    }

    void emitCall(const IR::Instruction &inst) {
        if (inst.op == "get_int") {
            out_ << "  li $v0, 5\n";
            out_ << "  syscall\n";
            storeValue(inst.result, "$v0");
            return;
        }
        if (inst.op == "get_char") {
            out_ << "  li $v0, 12\n";
            out_ << "  syscall\n";
            storeValue(inst.result, "$v0");
            return;
        }
        if (inst.op == "put_int" || inst.op == "put_char") {
            if (!inst.operands.empty()) {
                loadOperand(inst.operands.front(), "$a0");
            }
            out_ << "  li $v0, " << (inst.op == "put_int" ? 1 : 11) << "\n";
            out_ << "  syscall\n";
            return;
        }
        if (inst.op == "put_string" || inst.op == "put_str") {
            if (!inst.operands.empty()) {
                loadOperand(inst.operands.front(), "$a0");
            }
            out_ << "  li $v0, 4\n";
            out_ << "  syscall\n";
            return;
        }
        if (inst.op == "get_string") {
            if (!inst.operands.empty()) {
                loadOperand(inst.operands[0], "$a0");
            }
            if (inst.operands.size() > 1) {
                loadOperand(inst.operands[1], "$a1");
            }
            out_ << "  li $v0, 8\n";
            out_ << "  syscall\n";
            return;
        }

        static const char *argRegs[] = {"$a0", "$a1", "$a2", "$a3"};
        for (size_t i = 0; i < inst.operands.size() && i < 4; ++i) {
            loadOperand(inst.operands[i], argRegs[i]);
        }
        int extraCount = inst.operands.size() > 4 ? static_cast<int>(inst.operands.size() - 4) : 0;
        int extraBytes = extraCount * 4;
        for (size_t i = 4; i < inst.operands.size(); ++i) {
            loadOperand(inst.operands[i], "$t0");
            int offset = -extraBytes + static_cast<int>((i - 4) * 4);
            out_ << "  sw $t0, " << offset << "($sp)\n";
        }
        if (extraBytes > 0) {
            out_ << "  addiu $sp, $sp, -" << extraBytes << "\n";
        }
        out_ << "  jal " << inst.op << "\n";
        if (extraBytes > 0) {
            out_ << "  addiu $sp, $sp, " << extraBytes << "\n";
        }
        if (!inst.result.empty()) {
            storeValue(inst.result, "$v0");
        }
    }

    void emitReturn(const IR::Instruction &inst) {
        if (function_.name == "main") {
            if (!inst.operands.empty()) {
                loadOperand(inst.operands.front(), "$a0");
                out_ << "  li $v0, 17\n";
            } else {
                out_ << "  li $v0, 10\n";
            }
            out_ << "  syscall\n";
            return;
        }
        if (!inst.operands.empty()) {
            loadOperand(inst.operands.front(), "$v0");
        }
        restoreSavedRegs();
        out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
        out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
        out_ << "  jr $ra\n";
    }

    void emitDefaultReturn() {
        if (function_.name == "main") {
            out_ << "  li $v0, 10\n";
            out_ << "  syscall\n";
        } else {
            restoreSavedRegs();
            out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
            out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
            out_ << "  jr $ra\n";
        }
    }

    void restoreSavedRegs() {
        for (const auto &[reg, offset]: frame_.savedRegs) {
            out_ << "  lw " << reg << ", " << offset << "($sp)\n";
        }
    }
};

} // namespace

void Generator::emitModule(const IR::Module &module, std::ostream &out) {
    out << "# MIPS generated from toy LLVM-like IR\n";
    out << ".data\n";
    for (const auto &global: module.globals) {
        out << global.name << ": ";
        if (global.elementType == Type::Char) {
            out << ".byte ";
        } else {
            out << ".word ";
        }
        if (global.init.empty()) {
            out << "0";
        } else {
            for (size_t i = 0; i < global.init.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << global.init[i];
            }
        }
        out << "\n";
    }
    out << "\n.text\n";
    emitRuntimeStubs(out);
    out << "  j main\n";
    for (const auto &function: module.functions) {
        emitFunction(*function, out);
    }
}

void Generator::emitRuntimeStubs(std::ostream &out) {
    out << "# Builtins are emitted inline as syscalls.\n";
}

void Generator::emitFunction(const IR::Function &function, std::ostream &out) {
    FunctionEmitter(function, out).emit();
}

std::string generate(const IR::Module &module) {
    std::ostringstream out;
    Generator generator;
    generator.emitModule(module, out);
    return optimizeAssembly(out.str());
}

} // namespace MIPS
