#include "IR/Passes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace IR {
namespace {

Type typeFromIR(const std::string &type) {
    return type == "i8" ? Type::Char : Type::Int;
}

std::string edgeKey(const std::string &block, const std::string &var) {
    return block + "\n" + var;
}

bool sameOperand(const Operand &lhs, const Operand &rhs) {
    return lhs.type == rhs.type && lhs.text == rhs.text;
}

struct PhiSite {
    std::string block;
    std::string var;
    std::string result;
    Type type{Type::Int};
};

using State = std::unordered_map<std::string, Operand>;

class Mem2Reg {
public:
    void run(Function &function) {
        function_ = &function;
        collectCFG();
        collectPromotableAllocas();
        if (promotable_.empty()) {
            return;
        }
        rewriteBlocks();
        finalizePhiIncoming();
        simplifyTrivialPhis();
    }

private:
    Function *function_{nullptr};
    std::unordered_map<std::string, Type> promotable_;
    std::unordered_map<std::string, std::vector<std::string>> predecessors_;
    std::unordered_map<std::string, State> outStates_;
    std::unordered_map<std::string, Operand> replacements_;
    std::map<std::string, PhiSite> phiSites_;

    void collectCFG() {
        predecessors_.clear();
        for (const auto &block: function_->blocks) {
            predecessors_[block->name];
        }
        for (const auto &block: function_->blocks) {
            if (block->instructions.empty()) {
                continue;
            }
            const auto &term = block->instructions.back();
            if (term.opcode == Opcode::Br) {
                predecessors_[labelName(term.operands[0])].push_back(block->name);
            } else if (term.opcode == Opcode::CondBr) {
                predecessors_[labelName(term.operands[1])].push_back(block->name);
                predecessors_[labelName(term.operands[2])].push_back(block->name);
            }
        }
    }

    static std::string labelName(const Operand &operand) {
        std::string name = operand.text;
        if (!name.empty() && name.front() == '%') {
            name.erase(name.begin());
        }
        return name;
    }

    void collectPromotableAllocas() {
        std::unordered_set<std::string> rejected;
        for (const auto &block: function_->blocks) {
            for (const auto &inst: block->instructions) {
                if (inst.opcode == Opcode::Alloca && inst.operands.empty() && (inst.type == "i32" || inst.type == "i8")) {
                    promotable_[inst.result] = typeFromIR(inst.type);
                }
            }
        }
        for (const auto &block: function_->blocks) {
            for (const auto &inst: block->instructions) {
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    const auto &operand = inst.operands[i];
                    if (promotable_.find(operand.text) == promotable_.end()) {
                        continue;
                    }
                    bool allowedLoad = inst.opcode == Opcode::Load && i == 0;
                    bool allowedStore = inst.opcode == Opcode::Store && i == 1;
                    if (!allowedLoad && !allowedStore) {
                        rejected.insert(operand.text);
                    }
                }
            }
        }
        for (const auto &name: rejected) {
            promotable_.erase(name);
        }
    }

    void rewriteBlocks() {
        for (auto &blockPtr: function_->blocks) {
            auto &block = *blockPtr;
            State state = incomingState(block.name);
            std::vector<Instruction> rewritten;
            for (auto inst: block.instructions) {
                if (inst.opcode == Opcode::Alloca && promotable_.find(inst.result) != promotable_.end()) {
                    continue;
                }
                if (inst.opcode == Opcode::Load && isPromotedPtr(inst.operands[0])) {
                    replacements_[inst.result] = resolve(state[inst.operands[0].text]);
                    continue;
                }
                if (inst.opcode == Opcode::Store && isPromotedPtr(inst.operands[1])) {
                    state[inst.operands[1].text] = resolve(inst.operands[0]);
                    continue;
                }
                replaceOperands(inst);
                rewritten.push_back(std::move(inst));
            }
            block.instructions.clear();
            insertPhiInstructions(block);
            block.instructions.insert(block.instructions.end(),
                                      std::make_move_iterator(rewritten.begin()),
                                      std::make_move_iterator(rewritten.end()));
            outStates_[block.name] = std::move(state);
        }
    }

    State incomingState(const std::string &blockName) {
        State state;
        auto predsIt = predecessors_.find(blockName);
        const auto &preds = predsIt == predecessors_.end() ? emptyPreds_ : predsIt->second;
        for (const auto &[ptr, type]: promotable_) {
            if (preds.empty()) {
                state[ptr] = Operand::constant(type, 0);
            } else if (preds.size() == 1 && outStates_.find(preds.front()) != outStates_.end()) {
                state[ptr] = valueFromPred(preds.front(), ptr, type);
            } else {
                bool needsPhi = false;
                Operand first;
                bool firstSet = false;
                for (const auto &pred: preds) {
                    Operand value = valueFromPred(pred, ptr, type);
                    if (!firstSet) {
                        first = value;
                        firstSet = true;
                    } else if (!sameOperand(first, value)) {
                        needsPhi = true;
                    }
                    if (outStates_.find(pred) == outStates_.end()) {
                        needsPhi = true;
                    }
                }
                if (needsPhi) {
                    state[ptr] = ensurePhi(blockName, ptr, type);
                } else {
                    state[ptr] = first;
                }
            }
        }
        return state;
    }

    Operand valueFromPred(const std::string &pred, const std::string &ptr, Type type) {
        auto outIt = outStates_.find(pred);
        if (outIt == outStates_.end()) {
            return Operand::constant(type, 0);
        }
        auto valueIt = outIt->second.find(ptr);
        if (valueIt == outIt->second.end()) {
            return Operand::constant(type, 0);
        }
        return resolve(valueIt->second);
    }

    Operand ensurePhi(const std::string &blockName, const std::string &ptr, Type type) {
        std::string key = edgeKey(blockName, ptr);
        auto it = phiSites_.find(key);
        if (it == phiSites_.end()) {
            PhiSite site;
            site.block = blockName;
            site.var = ptr;
            site.result = function_->newTemp("phi");
            site.type = type;
            it = phiSites_.emplace(key, std::move(site)).first;
        }
        return Operand(typeToIR(type), it->second.result);
    }

    void insertPhiInstructions(BasicBlock &block) {
        std::vector<Instruction> phis;
        for (const auto &[key, site]: phiSites_) {
            (void) key;
            if (site.block == block.name) {
                phis.push_back(Instruction::phi(site.result, site.type, {}));
            }
        }
        block.instructions.insert(block.instructions.end(),
                                  std::make_move_iterator(phis.begin()),
                                  std::make_move_iterator(phis.end()));
    }

    void finalizePhiIncoming() {
        for (auto &blockPtr: function_->blocks) {
            auto &block = *blockPtr;
            for (auto &inst: block.instructions) {
                if (inst.opcode != Opcode::Phi) {
                    continue;
                }
                auto siteIt = findPhiSite(inst.result);
                if (siteIt == phiSites_.end()) {
                    continue;
                }
                const auto &site = siteIt->second;
                auto predsIt = predecessors_.find(block.name);
                if (predsIt == predecessors_.end()) {
                    continue;
                }
                inst.incoming.clear();
                for (const auto &pred: predsIt->second) {
                    inst.incoming.push_back({valueFromPred(pred, site.var, site.type), pred});
                }
            }
        }
    }

    void simplifyTrivialPhis() {
        bool changed = false;
        do {
            changed = false;
            for (auto &blockPtr: function_->blocks) {
                for (auto &inst: blockPtr->instructions) {
                    if (inst.opcode != Opcode::Phi || replacements_.find(inst.result) != replacements_.end()) {
                        continue;
                    }
                    Operand replacement;
                    if (trivialPhiReplacement(inst, replacement)) {
                        replacements_[inst.result] = resolve(std::move(replacement));
                        changed = true;
                    }
                }
            }
            if (changed) {
                rewriteAllOperands();
                removeReplacedPhis();
            }
        } while (changed);
    }

    bool trivialPhiReplacement(const Instruction &inst, Operand &replacement) {
        bool hasReplacement = false;
        for (const auto &incoming: inst.incoming) {
            Operand value = resolve(incoming.value);
            if (value.text == inst.result) {
                continue;
            }
            if (!hasReplacement) {
                replacement = std::move(value);
                hasReplacement = true;
            } else if (!sameOperand(replacement, value)) {
                return false;
            }
        }
        return hasReplacement;
    }

    void rewriteAllOperands() {
        for (auto &blockPtr: function_->blocks) {
            for (auto &inst: blockPtr->instructions) {
                replaceOperands(inst);
                for (auto &incoming: inst.incoming) {
                    incoming.value = resolve(std::move(incoming.value));
                }
            }
        }
    }

    void removeReplacedPhis() {
        for (auto &blockPtr: function_->blocks) {
            auto &instructions = blockPtr->instructions;
            instructions.erase(std::remove_if(instructions.begin(), instructions.end(), [this](const Instruction &inst) {
                return inst.opcode == Opcode::Phi && replacements_.find(inst.result) != replacements_.end();
            }), instructions.end());
        }
    }

    std::map<std::string, PhiSite>::iterator findPhiSite(const std::string &result) {
        for (auto it = phiSites_.begin(); it != phiSites_.end(); ++it) {
            if (it->second.result == result) {
                return it;
            }
        }
        return phiSites_.end();
    }

    bool isPromotedPtr(const Operand &operand) const {
        return promotable_.find(operand.text) != promotable_.end();
    }

    Operand resolve(Operand operand) {
        std::set<std::string> seen;
        while (!operand.text.empty() && replacements_.find(operand.text) != replacements_.end() && seen.insert(operand.text).second) {
            operand = replacements_[operand.text];
        }
        return operand;
    }

    void replaceOperands(Instruction &inst) {
        for (auto &operand: inst.operands) {
            operand = resolve(std::move(operand));
        }
    }

    inline static const std::vector<std::string> emptyPreds_{};
};

class ConstantPropagation {
public:
    void run(Function &function) {
        function_ = &function;
        bool changed;
        do {
            changed = false;
            rewriteAllOperands();
            for (auto &blockPtr: function_->blocks) {
                for (auto &inst: blockPtr->instructions) {
                    replaceOperands(inst);
                    Operand replacement;
                    if (tryFold(inst, replacement) && recordReplacement(inst.result, std::move(replacement))) {
                        changed = true;
                    }
                }
            }
        } while (changed);
        rewriteAllOperands();
        removeReplacedInstructions();
    }

private:
    Function *function_{nullptr};
    std::unordered_map<std::string, Operand> replacements_;

    bool tryFold(const Instruction &inst, Operand &replacement) {
        if (inst.result.empty()) {
            return false;
        }
        switch (inst.opcode) {
            case Opcode::Binary:
                return foldBinary(inst, replacement);
            case Opcode::ICmp:
                return foldICmp(inst, replacement);
            case Opcode::Cast:
                return foldCast(inst, replacement);
            case Opcode::Phi:
                return foldPhi(inst, replacement);
            default:
                return false;
        }
    }

    bool foldBinary(const Instruction &inst, Operand &replacement) {
        if (inst.operands.size() < 2) {
            return false;
        }
        Operand lhs = resolve(inst.operands[0]);
        Operand rhs = resolve(inst.operands[1]);
        int lhsValue = 0;
        int rhsValue = 0;
        bool lhsConst = parseInteger(lhs, lhsValue);
        bool rhsConst = parseInteger(rhs, rhsValue);
        if (lhsConst && rhsConst) {
            int result = 0;
            if (!evalBinary(inst.op, lhsValue, rhsValue, result)) {
                return false;
            }
            replacement = Operand(inst.type, std::to_string(maskForType(inst.type, result)));
            return true;
        }

        if (inst.op == "add") {
            if (rhsConst && rhsValue == 0) {
                replacement = lhs;
                return true;
            }
            if (lhsConst && lhsValue == 0) {
                replacement = rhs;
                return true;
            }
        } else if (inst.op == "sub") {
            if (rhsConst && rhsValue == 0) {
                replacement = lhs;
                return true;
            }
        } else if (inst.op == "mul") {
            if ((lhsConst && lhsValue == 0) || (rhsConst && rhsValue == 0)) {
                replacement = Operand(inst.type, "0");
                return true;
            }
            if (rhsConst && rhsValue == 1) {
                replacement = lhs;
                return true;
            }
            if (lhsConst && lhsValue == 1) {
                replacement = rhs;
                return true;
            }
        } else if (inst.op == "sdiv") {
            if (rhsConst && rhsValue == 1) {
                replacement = lhs;
                return true;
            }
            if (lhsConst && lhsValue == 0 && (!rhsConst || rhsValue != 0)) {
                replacement = Operand(inst.type, "0");
                return true;
            }
        } else if (inst.op == "srem") {
            if (rhsConst && rhsValue == 1) {
                replacement = Operand(inst.type, "0");
                return true;
            }
            if (lhsConst && lhsValue == 0 && (!rhsConst || rhsValue != 0)) {
                replacement = Operand(inst.type, "0");
                return true;
            }
        } else if (inst.op == "and") {
            if ((lhsConst && lhsValue == 0) || (rhsConst && rhsValue == 0)) {
                replacement = Operand(inst.type, "0");
                return true;
            }
        } else if (inst.op == "or") {
            if (rhsConst && rhsValue == 0) {
                replacement = lhs;
                return true;
            }
            if (lhsConst && lhsValue == 0) {
                replacement = rhs;
                return true;
            }
        }
        return false;
    }

    bool foldICmp(const Instruction &inst, Operand &replacement) {
        if (inst.operands.size() < 2) {
            return false;
        }
        Operand lhs = resolve(inst.operands[0]);
        Operand rhs = resolve(inst.operands[1]);
        int lhsValue = 0;
        int rhsValue = 0;
        if (parseInteger(lhs, lhsValue) && parseInteger(rhs, rhsValue)) {
            bool result = false;
            if (!evalICmp(inst.op, lhsValue, rhsValue, result)) {
                return false;
            }
            replacement = Operand(inst.type, result ? "1" : "0");
            return true;
        }
        if (sameOperand(lhs, rhs)) {
            if (inst.op == "eq" || inst.op == "sle" || inst.op == "sge") {
                replacement = Operand(inst.type, "1");
                return true;
            }
            if (inst.op == "ne" || inst.op == "slt" || inst.op == "sgt") {
                replacement = Operand(inst.type, "0");
                return true;
            }
        }
        return false;
    }

    bool foldCast(const Instruction &inst, Operand &replacement) {
        if (inst.operands.empty()) {
            return false;
        }
        Operand source = resolve(inst.operands.front());
        int value = 0;
        if (!parseInteger(source, value)) {
            if (source.type == inst.type) {
                replacement = source;
                return true;
            }
            return false;
        }
        if (inst.op == "trunc") {
            replacement = Operand(inst.type, std::to_string(maskForType(inst.type, value)));
            return true;
        }
        if (inst.op == "zext") {
            replacement = Operand(inst.type, std::to_string(maskForType(source.type, value)));
            return true;
        }
        return false;
    }

    bool foldPhi(const Instruction &inst, Operand &replacement) {
        bool hasValue = false;
        for (const auto &incoming: inst.incoming) {
            Operand value = resolve(incoming.value);
            if (value.text == inst.result) {
                continue;
            }
            if (!hasValue) {
                replacement = std::move(value);
                hasValue = true;
            } else if (!sameOperand(replacement, value)) {
                return false;
            }
        }
        return hasValue;
    }

    bool recordReplacement(const std::string &result, Operand replacement) {
        if (replacement.type == "label" || replacement.text.empty() || replacement.text == result) {
            return false;
        }
        replacement = resolve(std::move(replacement));
        if (replacement.text == result) {
            return false;
        }
        auto it = replacements_.find(result);
        if (it != replacements_.end() && sameOperand(it->second, replacement)) {
            return false;
        }
        replacements_[result] = std::move(replacement);
        return true;
    }

    Operand resolve(Operand operand) const {
        if (operand.type == "label") {
            return operand;
        }
        std::set<std::string> seen;
        while (!operand.text.empty() && replacements_.find(operand.text) != replacements_.end() && seen.insert(operand.text).second) {
            operand = replacements_.at(operand.text);
        }
        return operand;
    }

    void replaceOperands(Instruction &inst) const {
        for (auto &operand: inst.operands) {
            operand = resolve(std::move(operand));
        }
        for (auto &incoming: inst.incoming) {
            incoming.value = resolve(std::move(incoming.value));
        }
    }

    void rewriteAllOperands() {
        for (auto &blockPtr: function_->blocks) {
            for (auto &inst: blockPtr->instructions) {
                replaceOperands(inst);
            }
        }
    }

    void removeReplacedInstructions() {
        for (auto &blockPtr: function_->blocks) {
            auto &instructions = blockPtr->instructions;
            instructions.erase(std::remove_if(instructions.begin(), instructions.end(), [this](const Instruction &inst) {
                return isRemovable(inst) && replacements_.find(inst.result) != replacements_.end();
            }), instructions.end());
        }
    }

    static bool isRemovable(const Instruction &inst) {
        return inst.opcode == Opcode::Binary || inst.opcode == Opcode::ICmp ||
               inst.opcode == Opcode::Cast || inst.opcode == Opcode::Phi;
    }

    static bool parseInteger(const Operand &operand, int &value) {
        if (operand.text.empty() || operand.text.front() == '%' || operand.text.front() == '@') {
            return false;
        }
        size_t pos = operand.text[0] == '-' ? 1 : 0;
        if (pos == operand.text.size()) {
            return false;
        }
        for (; pos < operand.text.size(); ++pos) {
            if (!std::isdigit(static_cast<unsigned char>(operand.text[pos]))) {
                return false;
            }
        }
        try {
            value = std::stoi(operand.text);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    static bool evalBinary(const std::string &op, int lhs, int rhs, int &result) {
        if (op == "add") {
            result = wrapI32(static_cast<long long>(lhs) + rhs);
        } else if (op == "sub") {
            result = wrapI32(static_cast<long long>(lhs) - rhs);
        } else if (op == "mul") {
            result = wrapI32(static_cast<long long>(lhs) * rhs);
        } else if (op == "sdiv") {
            if (rhs == 0 || (lhs == std::numeric_limits<int>::min() && rhs == -1)) {
                return false;
            }
            result = lhs / rhs;
        } else if (op == "srem") {
            if (rhs == 0 || (lhs == std::numeric_limits<int>::min() && rhs == -1)) {
                return false;
            }
            result = lhs % rhs;
        } else if (op == "and") {
            result = lhs & rhs;
        } else if (op == "or") {
            result = lhs | rhs;
        } else {
            return false;
        }
        return true;
    }

    static bool evalICmp(const std::string &op, int lhs, int rhs, bool &result) {
        if (op == "slt") {
            result = lhs < rhs;
        } else if (op == "sgt") {
            result = lhs > rhs;
        } else if (op == "sle") {
            result = lhs <= rhs;
        } else if (op == "sge") {
            result = lhs >= rhs;
        } else if (op == "eq") {
            result = lhs == rhs;
        } else if (op == "ne") {
            result = lhs != rhs;
        } else {
            return false;
        }
        return true;
    }

    static int maskForType(const std::string &type, int value) {
        if (type == "i8") {
            return value & 0xff;
        }
        if (type == "i1") {
            return value & 1;
        }
        return value;
    }

    static int wrapI32(long long value) {
        std::uint32_t wrapped = static_cast<std::uint32_t>(value);
        long long signedValue = wrapped <= 0x7fffffffU ? wrapped : static_cast<long long>(wrapped) - 0x100000000LL;
        return static_cast<int>(signedValue);
    }
};

} // namespace

void runScalarMem2Reg(Module &module) {
    for (auto &function: module.functions) {
        Mem2Reg().run(*function);
    }
}

void runConstantPropagation(Module &module) {
    for (auto &function: module.functions) {
        ConstantPropagation().run(*function);
    }
}

} // namespace IR
