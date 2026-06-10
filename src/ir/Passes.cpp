#include "ir/Passes.h"

#include <algorithm>
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

} // namespace

void runScalarMem2Reg(Module &module) {
    for (auto &function: module.functions) {
        Mem2Reg().run(*function);
    }
}

} // namespace IR
