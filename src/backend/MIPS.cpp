#include "backend/MIPS.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
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
    std::unordered_map<std::string, std::string> labels;
    int nextOffset{0};
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
        spillParameters();
        for (const auto &block: function_.blocks) {
            currentBlock_ = block->name;
            out_ << labelOf(block->name) << ":\n";
            for (const auto &inst: block->instructions) {
                emitInst(inst);
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
    std::string currentBlock_;
    int edgeId_{0};

    int reserveSlot(int bytes = 4) {
        frame_.nextOffset = alignTo(frame_.nextOffset, 4);
        int offset = frame_.nextOffset;
        frame_.nextOffset += alignTo(bytes, 4);
        return offset;
    }

    void buildFrame() {
        for (const auto &param: function_.params) {
            frame_.valueSlots["%" + param.name] = reserveSlot();
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
                    frame_.valueSlots[inst.result] = reserveSlot();
                }
            }
        }
        frame_.frameSize = alignTo(frame_.nextOffset + 8, 8);
        collectPhiMoves();
    }

    void collectPhiMoves() {
        for (const auto &block: function_.blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode != IR::Opcode::Phi) {
                    continue;
                }
                for (const auto &incoming: inst.incoming) {
                    phiMoves_[edgeKey(incoming.block, block->name)].push_back({incoming.value, inst.result});
                }
            }
        }
    }

    void spillParameters() {
        static const char *argRegs[] = {"$a0", "$a1", "$a2", "$a3"};
        for (size_t i = 0; i < function_.params.size() && i < 4; ++i) {
            auto name = "%" + function_.params[i].name;
            out_ << "  sw " << argRegs[i] << ", " << frame_.valueSlots[name] << "($sp)\n";
        }
        if (function_.params.size() > 4) {
            out_ << "  # TODO: load stack-passed parameters\n";
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

    void emitPhiMoves(const std::string &target) {
        auto it = phiMoves_.find(edgeKey(currentBlock_, target));
        if (it == phiMoves_.end()) {
            return;
        }
        for (const auto &[value, result]: it->second) {
            loadOperand(value, "$t8");
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
            out_ << "  " << (operand.type == "i8" ? "lb" : "lw") << " " << reg << ", " << global << "\n";
        } else {
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
        auto it = frame_.valueSlots.find(name);
        if (it != frame_.valueSlots.end()) {
            out_ << "  sw " << reg << ", " << it->second << "($sp)\n";
        }
    }

    void emitLoad(const IR::Instruction &inst) {
        const auto &ptr = inst.operands.front();
        if (!ptr.text.empty() && ptr.text.front() == '@') {
            out_ << "  " << (inst.type == "i8" ? "lb" : "lw") << " $t0, " << stripPrefix(ptr.text) << "\n";
        } else if (frame_.pointerSlots.find(ptr.text) != frame_.pointerSlots.end()) {
            auto it = frame_.pointerSlots.find(ptr.text);
            out_ << "  " << (inst.type == "i8" ? "lb" : "lw") << " $t0, " << it->second << "($sp)\n";
        } else {
            loadPointerAddress(ptr, "$t9");
            out_ << "  " << (inst.type == "i8" ? "lb" : "lw") << " $t0, 0($t9)\n";
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
        storeValue(inst.result, "$t2");
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
        if (inst.operands.size() > 4) {
            out_ << "  # TODO: pass arguments beyond $a3\n";
        }
        out_ << "  jal " << inst.op << "\n";
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
        out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
        out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
        out_ << "  jr $ra\n";
    }

    void emitDefaultReturn() {
        if (function_.name == "main") {
            out_ << "  li $v0, 10\n";
            out_ << "  syscall\n";
        } else {
            out_ << "  lw $ra, " << frame_.frameSize - 4 << "($sp)\n";
            out_ << "  addiu $sp, $sp, " << frame_.frameSize << "\n";
            out_ << "  jr $ra\n";
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
