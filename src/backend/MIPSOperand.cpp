#include "backend/MIPSInternal.h"

namespace MIPS::detail {

std::string FunctionEmitter::assignedRegister(const std::string &name) const {
    auto it = frame_.valueRegs.find(name);
    return it == frame_.valueRegs.end() ? "" : it->second;
}

std::string FunctionEmitter::resultRegister(const std::string &name, const std::string &fallback) const {
    std::string reg = assignedRegister(name);
    return reg.empty() ? fallback : reg;
}

std::string FunctionEmitter::materializeOperand(const IR::Operand &operand, const std::string &scratch) {
    if (!operand.text.empty() && operand.text.front() == '%' && operand.type != "ptr") {
        std::string reg = assignedRegister(operand.text);
        if (!reg.empty()) {
            return reg;
        }
    }
    if (operand.type == "ptr") {
        return materializePointerAddress(operand, scratch);
    }
    loadOperand(operand, scratch);
    return scratch;
}

std::string FunctionEmitter::materializePointerAddress(const IR::Operand &operand, const std::string &scratch) {
    if (!operand.text.empty() && operand.text.front() == '%') {
        std::string reg = assignedRegister(operand.text);
        if (!reg.empty()) {
            return reg;
        }
    }
    loadPointerAddress(operand, scratch);
    return scratch;
}

void FunctionEmitter::loadOperand(const IR::Operand &operand, const std::string &reg) {
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

void FunctionEmitter::loadPointerAddress(const IR::Operand &operand, const std::string &reg) {
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

void FunctionEmitter::storeValue(const std::string &name, const std::string &reg) {
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

} // namespace MIPS::detail
