//
// Created by Steel_Shadow on 2023/10/26.
//
#include "IR.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "config.h"
#include "errorHandler/Error.h"
#include "middle/Analysis.h"
#include "middle/IRUtils.h"
#include "middle/Optimize.h"

using namespace IR;

std::ofstream IR::IRFileStream;
std::vector<std::string> Str::MIPS_strings;
int Str::idAllocator = 0;
int Label::idAllocator = 0;
int Function::idAllocator = 0;

namespace {
struct ConstInfo {
    int value{};
    Type type{Type::Int};
};

std::string typeKey(Type type) {
    return std::to_string(static_cast<int>(type));
}

void writeIRLine(const std::string &line) {
#if defined(STDOUT_IR)
    std::cout << line << '\n';
#endif
#if defined(FILEOUT_IR)
    IRFileStream << line << '\n';
#endif
}

std::string llvmType(Type type) {
    switch (type) {
        case Type::Void:
            return "void";
        case Type::Int:
            return "i32";
        case Type::Char:
            return "i8";
        case Type::IntPtr:
        case Type::CharPtr:
            return "ptr";
        default:
            Error::raise("Bad Type in llvmType");
            return "void";
    }
}

std::string valueLLVMType(Type type) {
    return isPtrType(type) ? "ptr" : llvmType(type);
}

std::string escapedName(const std::string &name) {
    return name;
}

std::string varLLVMName(const Var &var) {
    if (var.depth == 0) {
        return "@" + escapedName(var.name);
    }
    return "%" + escapedName(var.name) + "." + std::to_string(var.depth);
}

std::string labelLLVMName(const Label &label) {
    return "%" + label.nameAndId;
}

std::string valueLLVMName(const Element *element) {
    if (!element) {
        return "void";
    }
    if (auto temp = dynamic_cast<const Temp *>(element)) {
        return temp->toString();
    }
    if (auto value = dynamic_cast<const ConstVal *>(element)) {
        return std::to_string(value->value);
    }
    if (auto var = dynamic_cast<const Var *>(element)) {
        return varLLVMName(*var);
    }
    if (auto label = dynamic_cast<const Label *>(element)) {
        return labelLLVMName(*label);
    }
    if (auto str = dynamic_cast<const Str *>(element)) {
        return "@" + str->toString();
    }
    return element->toString();
}

Type elementType(const Element *element, Type fallback = Type::Int) {
    if (auto temp = dynamic_cast<const Temp *>(element)) {
        return temp->type;
    }
    if (auto value = dynamic_cast<const ConstVal *>(element)) {
        return value->type;
    }
    if (auto var = dynamic_cast<const Var *>(element)) {
        return var->type;
    }
    return fallback;
}

Temp *asMutableTemp(const std::unique_ptr<Element> &element) {
    return dynamic_cast<Temp *>(element.get());
}

bool isPromotableScalarLocal(const Var &var) {
    return var.depth > 0 && var.dims.empty() && !var.storesAddress;
}

bool getConstant(const Element *element,
                 const std::unordered_map<int, ConstInfo> &constants,
                 ConstInfo &result) {
    if (auto value = dynamic_cast<const ConstVal *>(element)) {
        result = {value->value, value->type};
        return true;
    }
    auto temp = dynamic_cast<const Temp *>(element);
    if (!temp || temp->id < 0) {
        return false;
    }
    auto iter = constants.find(temp->id);
    if (iter == constants.end()) {
        return false;
    }
    result = iter->second;
    return true;
}

bool evalBinary(Op op, int lhs, int rhs, int &out) {
    switch (op) {
        case Op::Add:
            out = lhs + rhs;
            return true;
        case Op::Sub:
            out = lhs - rhs;
            return true;
        case Op::Mul:
            out = lhs * rhs;
            return true;
        case Op::Div:
            if (rhs == 0) {
                return false;
            }
            out = lhs / rhs;
            return true;
        case Op::Mod:
            if (rhs == 0) {
                return false;
            }
            out = lhs % rhs;
            return true;
        case Op::And:
            out = lhs & rhs;
            return true;
        case Op::Or:
            out = lhs | rhs;
            return true;
        case Op::Xor:
            out = lhs ^ rhs;
            return true;
        case Op::XorLimb: {
            const std::uint32_t lhsMagnitude = lhs < 0
                                                       ? 0U - static_cast<std::uint32_t>(lhs)
                                                       : static_cast<std::uint32_t>(lhs);
            const std::uint32_t rhsMagnitude = rhs < 0
                                                       ? 0U - static_cast<std::uint32_t>(rhs)
                                                       : static_cast<std::uint32_t>(rhs);
            out = static_cast<int>(
                    (((lhs < 0) == (rhs < 0)
                              ? lhsMagnitude ^ rhsMagnitude
                              : lhsMagnitude | rhsMagnitude)
                     & 65535U));
            return true;
        }
        case Op::AndLimb:
            out = lhs >= 0 && rhs >= 0 ? (lhs & rhs & 65535) : 0;
            return true;
        case Op::Leq:
            out = lhs <= rhs;
            return true;
        case Op::Lss:
            out = lhs < rhs;
            return true;
        case Op::Geq:
            out = lhs >= rhs;
            return true;
        case Op::Gre:
            out = lhs > rhs;
            return true;
        case Op::Eql:
            out = lhs == rhs;
            return true;
        case Op::Neq:
            out = lhs != rhs;
            return true;
        default:
            return false;
    }
}

bool isBinaryInst(Op op) {
    switch (op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::XorLimb:
        case Op::AndLimb:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
            return true;
        default:
            return false;
    }
}

void replaceWithLoadImm(Inst &inst, int value, Type type) {
    auto res = inst.res ? inst.res->clone() : nullptr;
    inst = Inst(Op::LoadImd,
                std::move(res),
                std::make_unique<ConstVal>(normalizeForType(value, type), type),
                nullptr);
}

void replaceWithMove(Inst &inst, const Temp *source) {
    auto res = inst.res ? inst.res->clone() : nullptr;
    inst = Inst(Op::NewMove,
                std::move(res),
                source ? source->clone() : nullptr,
                nullptr);
}

void updateKnownConstant(const Inst &inst, std::unordered_map<int, ConstInfo> &constants) {
    if (!inst.definesTemp()) {
        return;
    }
    const auto *res = asTemp(inst.res);
    if (!res || res->id < 0) {
        return;
    }

    ConstInfo known{};
    if (inst.op == Op::LoadImd && getConstant(inst.arg1.get(), constants, known)) {
        known.value = normalizeForType(known.value, res->type);
        known.type = res->type;
        constants[res->id] = known;
        return;
    }
    if (inst.op == Op::NewMove && getConstant(inst.arg1.get(), constants, known)) {
        known.value = normalizeForType(known.value, res->type);
        known.type = res->type;
        constants[res->id] = known;
        return;
    }
    constants.erase(res->id);
}

bool simplifyInst(Inst &inst, const std::unordered_map<int, ConstInfo> &constants) {
    auto *res = asMutableTemp(inst.res);
    if (!res || res->id < 0) {
        return false;
    }

    ConstInfo lhs{};
    ConstInfo rhs{};
    const bool lhsConst = getConstant(inst.arg1.get(), constants, lhs);
    const bool rhsConst = getConstant(inst.arg2.get(), constants, rhs);

    if (inst.op == Op::LoadImd) {
        if (auto imm = asConstant(inst.arg1)) {
            const int normalized = normalizeForType(imm->value, res->type);
            if (normalized != imm->value || imm->type != res->type) {
                replaceWithLoadImm(inst, normalized, res->type);
                return true;
            }
        }
        return false;
    }

    if (inst.op == Op::NewMove) {
        if (lhsConst) {
            replaceWithLoadImm(inst, lhs.value, res->type);
            return true;
        }
        return false;
    }

    if (inst.op == Op::Neg && lhsConst) {
        replaceWithLoadImm(inst, -lhs.value, res->type);
        return true;
    }

    if (inst.op == Op::Not && lhsConst) {
        replaceWithLoadImm(inst, !lhs.value, Type::Int);
        return true;
    }

    if (isBinaryInst(inst.op)) {
        if (lhsConst && rhsConst) {
            int value = 0;
            if (evalBinary(inst.op, lhs.value, rhs.value, value)) {
                replaceWithLoadImm(inst, value, res->type);
                return true;
            }
        }

        const auto *lhsTemp = asTemp(inst.arg1);
        const auto *rhsTemp = asTemp(inst.arg2);
        if (inst.op == Op::Add) {
            if (rhsConst && rhs.value == 0 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
            if (lhsConst && lhs.value == 0 && rhsTemp) {
                replaceWithMove(inst, rhsTemp);
                return true;
            }
        } else if (inst.op == Op::Sub) {
            if (rhsConst && rhs.value == 0 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
        } else if (inst.op == Op::Mul) {
            if ((lhsConst && lhs.value == 0) || (rhsConst && rhs.value == 0)) {
                replaceWithLoadImm(inst, 0, res->type);
                return true;
            }
            if (rhsConst && rhs.value == 1 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
            if (lhsConst && lhs.value == 1 && rhsTemp) {
                replaceWithMove(inst, rhsTemp);
                return true;
            }
        } else if (inst.op == Op::Div) {
            if (rhsConst && rhs.value == 1 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
        } else if (inst.op == Op::Mod) {
            if (rhsConst && rhs.value == 1) {
                replaceWithLoadImm(inst, 0, res->type);
                return true;
            }
        } else if (inst.op == Op::And) {
            if ((lhsConst && lhs.value == 0) || (rhsConst && rhs.value == 0)) {
                replaceWithLoadImm(inst, 0, res->type);
                return true;
            }
        } else if (inst.op == Op::Or) {
            if (rhsConst && rhs.value == 0 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
        } else if (inst.op == Op::Xor) {
            if (rhsConst && rhs.value == 0 && lhsTemp) {
                replaceWithMove(inst, lhsTemp);
                return true;
            }
            if (lhsConst && lhs.value == 0 && rhsTemp) {
                replaceWithMove(inst, rhsTemp);
                return true;
            }
        }
    }

    if (inst.op == Op::MulImd) {
        auto imm = asConstant(inst.arg2);
        if (!imm) {
            return false;
        }
        if (lhsConst) {
            replaceWithLoadImm(inst, lhs.value * imm->value, res->type);
            return true;
        }
        auto *lhsTemp = asMutableTemp(inst.arg1);
        if (imm->value == 0) {
            replaceWithLoadImm(inst, 0, res->type);
            return true;
        }
        if (imm->value == 1 && lhsTemp) {
            replaceWithMove(inst, lhsTemp);
            return true;
        }
    }

    if (inst.op == Op::Mult4 && lhsConst) {
        replaceWithLoadImm(inst, lhs.value * 4, res->type);
        return true;
    }

    return false;
}

bool simplifyConstants(Function &function) {
    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        std::unordered_map<int, ConstInfo> constants;
        std::unordered_map<Var, ConstInfo> memoryConstants;
        auto &instructions = block->instructions;
        for (size_t i = 0; i < instructions.size();) {
            auto &inst = instructions[i];

            if (inst.op == Op::Load) {
                auto *res = asMutableTemp(inst.res);
                auto *var = asVar(inst.arg1);
                if (res && var && !inst.arg2 && isPromotableScalarLocal(*var)) {
                    auto iter = memoryConstants.find(*var);
                    if (iter != memoryConstants.end()) {
                        replaceWithLoadImm(inst, iter->second.value, res->type);
                        changed = true;
                    }
                }
            }

            if ((inst.op == Op::Bif0 || inst.op == Op::Bif1)) {
                ConstInfo cond{};
                if (getConstant(inst.arg1.get(), constants, cond)) {
                    const bool taken = inst.op == Op::Bif1 ? cond.value != 0 : cond.value == 0;
                    if (taken) {
                        inst = Inst(Op::Br, nullptr, inst.arg2 ? inst.arg2->clone() : nullptr, nullptr);
                        ++i;
                    } else {
                        instructions.erase(instructions.begin() + static_cast<long>(i));
                    }
                    changed = true;
                    continue;
                }
            }

            if (simplifyInst(inst, constants)) {
                changed = true;
            }
            updateKnownConstant(inst, constants);

            if (inst.op == Op::Store) {
                auto *var = asVar(inst.arg1);
                if (var && !inst.arg2 && isPromotableScalarLocal(*var)) {
                    ConstInfo stored{};
                    if (getConstant(inst.res.get(), constants, stored)) {
                        stored.value = normalizeForType(stored.value, var->type);
                        stored.type = ptrToValue(var->type);
                        memoryConstants[*var] = stored;
                    } else {
                        memoryConstants.erase(*var);
                    }
                }
            } else if (inst.op == Op::StoreDynamic || inst.op == Op::MemZero
                       || inst.op == Op::Call || inst.op == Op::GetString
                       || inst.op == Op::InStack || inst.op == Op::OutStack) {
                memoryConstants.clear();
            }
            ++i;
        }
    }
    return changed;
}

bool eliminateDeadCode(Function &function) {
    bool changed = false;
    bool localChange = true;
    while (localChange) {
        localChange = false;
        auto useDef = function.buildUseDefChains();

        for (auto &block: function.getMutableBasicBlocks()) {
            auto &instructions = block->instructions;
            for (size_t i = 0; i < instructions.size();) {
                const auto &inst = instructions[i];
                const auto *res = asTemp(inst.res);
                const auto chain = res ? useDef.find(res->valueKey()) : useDef.end();
                const bool unused = chain == useDef.end() || chain->second.uses.empty();
                if (inst.isRemovableIfUnused() && res && unused) {
                    instructions.erase(instructions.begin() + static_cast<long>(i));
                    localChange = true;
                    changed = true;
                    continue;
                }
                ++i;
            }
        }
    }
    return changed;
}

bool cleanupAfterTerminators(Function &function) {
    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        bool afterTerminator = false;
        auto &instructions = block->instructions;
        for (size_t i = 0; i < instructions.size();) {
            auto &inst = instructions[i];
            if (afterTerminator && !inst.isScopeMarker()) {
                instructions.erase(instructions.begin() + static_cast<long>(i));
                changed = true;
                continue;
            }
            if (inst.isUnconditionalTerminator()) {
                afterTerminator = true;
            }
            ++i;
        }
    }
    return changed;
}

void addLabelSuccessor(const std::unique_ptr<Element> &element,
                       const std::unordered_map<std::string, size_t> &labelToIndex,
                       std::vector<size_t> &successors) {
    auto label = asLabel(element);
    if (!label) {
        return;
    }
    auto iter = labelToIndex.find(label->nameAndId);
    if (iter != labelToIndex.end()) {
        successors.push_back(iter->second);
    }
}

bool removeUnreachableBlocks(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }

    std::unordered_map<std::string, size_t> labelToIndex;
    for (size_t i = 0; i < blocks.size(); ++i) {
        labelToIndex[blocks[i]->label.nameAndId] = i;
    }

    std::vector<bool> reachable(blocks.size(), false);
    std::queue<size_t> work;
    reachable[0] = true;
    work.push(0);

    while (!work.empty()) {
        const size_t index = work.front();
        work.pop();

        std::vector<size_t> successors;
        bool hasHardTerminator = false;
        for (const auto &inst: blocks[index]->instructions) {
            if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                addLabelSuccessor(inst.arg2, labelToIndex, successors);
            } else if (inst.op == Op::Br) {
                addLabelSuccessor(inst.arg1, labelToIndex, successors);
                hasHardTerminator = true;
                break;
            } else if (inst.op == Op::Ret || inst.op == Op::RetMain) {
                hasHardTerminator = true;
                break;
            }
        }
        if (!hasHardTerminator && index + 1 < blocks.size()) {
            successors.push_back(index + 1);
        }

        for (size_t successor: successors) {
            if (!reachable[successor]) {
                reachable[successor] = true;
                work.push(successor);
            }
        }
    }

    const size_t oldSize = blocks.size();
    BasicBlocks kept;
    kept.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (reachable[i]) {
            kept.emplace_back(std::move(blocks[i]));
        }
    }
    blocks = std::move(kept);
    return blocks.size() != oldSize;
}

struct VarUseInfo {
    bool hasAnyUse{false};
    bool hasReadOrAddressUse{false};
};

bool isOwnedLocalMemory(const Var &var) {
    return var.depth > 0 && !var.storesAddress;
}

void markVarUse(const std::unique_ptr<Element> &element,
                std::unordered_map<Var, VarUseInfo> &uses,
                bool readOrAddressUse) {
    auto var = asVar(element);
    if (!var || !isOwnedLocalMemory(*var)) {
        return;
    }
    auto &info = uses[*var];
    info.hasAnyUse = true;
    info.hasReadOrAddressUse = info.hasReadOrAddressUse || readOrAddressUse;
}

std::unordered_map<Var, VarUseInfo> collectLocalMemoryUses(const Function &function) {
    std::unordered_map<Var, VarUseInfo> uses;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            switch (inst.op) {
                case Op::Alloca:
                    break;
                case Op::Store:
                case Op::StoreDynamic:
                case Op::MemZero:
                    markVarUse(inst.arg1, uses, false);
                    break;
                case Op::Load:
                case Op::LoadDynamic:
                    markVarUse(inst.arg1, uses, true);
                    break;
                case Op::PushAddressParam:
                case Op::PrintStr:
                case Op::GetString:
                    markVarUse(inst.arg1, uses, true);
                    break;
                default:
                    break;
            }
        }
    }
    return uses;
}

bool cleanupScalarMemory(Function &function) {
    bool changed = false;

    auto uses = collectLocalMemoryUses(function);
    for (auto &block: function.getMutableBasicBlocks()) {
        auto &instructions = block->instructions;
        for (size_t i = 0; i < instructions.size();) {
            auto &inst = instructions[i];
            auto *var = asVar(inst.arg1);
            if ((inst.op == Op::Store || inst.op == Op::StoreDynamic
                 || inst.op == Op::MemZero)
                && var && isOwnedLocalMemory(*var)
                && !uses[*var].hasReadOrAddressUse) {
                instructions.erase(instructions.begin() + static_cast<long>(i));
                changed = true;
                continue;
            }
            ++i;
        }
    }

    if (changed) {
        uses = collectLocalMemoryUses(function);
    }
    for (auto &block: function.getMutableBasicBlocks()) {
        auto &instructions = block->instructions;
        for (size_t i = 0; i < instructions.size();) {
            auto &inst = instructions[i];
            auto *var = asVar(inst.arg1);
            if (inst.op == Op::Alloca && var && isOwnedLocalMemory(*var)
                && !uses[*var].hasAnyUse) {
                instructions.erase(instructions.begin() + static_cast<long>(i));
                changed = true;
                continue;
            }
            ++i;
        }
    }

    return changed;
}

bool propagateConstantGlobals(
        Function &function,
        const std::vector<std::pair<std::string, GlobVar>> &globals,
        const std::unordered_set<std::string> &readOnlyGlobals) {
    std::unordered_map<std::string, const GlobVar *> constants;
    for (const auto &[name, global]: globals) {
        if ((global.cons || readOnlyGlobals.count(name) != 0)
            && !global.initVal.empty()) {
            constants.emplace(name, &global);
        }
    }

    std::unordered_map<int, ConstInfo> knownConstants;
    bool discovered = true;
    while (discovered) {
        const size_t previousSize = knownConstants.size();
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                updateKnownConstant(inst, knownConstants);
            }
        }
        discovered = knownConstants.size() != previousSize;
    }

    bool changed = false;
    for (auto &block: function.getMutableBasicBlocks()) {
        for (auto &inst: block->instructions) {
            const auto *var = asVar(inst.arg1);
            const auto *result = asTemp(inst.res);
            if ((inst.op != Op::Load && inst.op != Op::LoadDynamic)
                || !var || var->depth != 0 || !result) {
                continue;
            }
            auto constant = constants.find(var->name);
            if (constant == constants.end()) {
                continue;
            }

            int index = 0;
            if (inst.arg2) {
                ConstInfo offset{};
                if (!getConstant(inst.arg2.get(), knownConstants, offset)) {
                    continue;
                }
                index = offset.value;
                if (inst.op == Op::LoadDynamic) {
                    const int elementSize = sizeOfType(ptrToValue(constant->second->type));
                    if (elementSize <= 0 || index % elementSize != 0) {
                        continue;
                    }
                    index /= elementSize;
                }
            }
            if (index < 0
                || static_cast<size_t>(index) >= constant->second->initVal.size()) {
                continue;
            }
            replaceWithLoadImm(
                    inst, constant->second->initVal[static_cast<size_t>(index)], result->type);
            changed = true;
        }
    }
    return changed;
}

std::unordered_set<std::string> findReadOnlyGlobals(
        const std::vector<std::pair<std::string, GlobVar>> &globals,
        const Function *mainFunction,
        const std::vector<std::unique_ptr<Function>> &functions) {
    std::unordered_set<std::string> readOnly;
    for (const auto &[name, global]: globals) {
        (void) global;
        readOnly.insert(name);
    }

    auto inspect = [&](const Function &function) {
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                if (inst.op != Op::Store && inst.op != Op::StoreDynamic
                    && inst.op != Op::MemZero
                    && inst.op != Op::GetString
                    && inst.op != Op::PushAddressParam) {
                    continue;
                }
                const auto *var = asVar(inst.arg1);
                if (var && var->depth == 0) {
                    readOnly.erase(var->name);
                }
            }
        }
    };
    if (mainFunction) {
        inspect(*mainFunction);
    }
    for (const auto &function: functions) {
        inspect(*function);
    }
    return readOnly;
}

bool poolEntryConstants(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }

    std::unordered_map<std::string, Temp> canonical;
    bool changed = false;
    auto &instructions = blocks.front()->instructions;
    for (size_t index = 0; index < instructions.size();) {
        auto &inst = instructions[index];
        const auto *result = asTemp(inst.res);
        const auto *constant = asConstant(inst.arg1);
        if (inst.op != Op::LoadImd || !result || result->id < 0 || !constant) {
            ++index;
            continue;
        }
        const std::string key = typeKey(constant->type) + ":" + std::to_string(constant->value);
        auto existing = canonical.find(key);
        if (existing == canonical.end()) {
            canonical.emplace(key, *result);
            ++index;
            continue;
        }
        function.replaceAllUsesWith(*result, existing->second);
        instructions.erase(instructions.begin() + static_cast<long>(index));
        changed = true;
    }
    return changed;
}

class FunctionPass {
public:
    virtual ~FunctionPass() = default;
    virtual bool run(Function &function) = 0;
};

class ConstantFoldPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return simplifyConstants(function);
    }
};

class GlobalPropagationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return propagateGlobalConstantsAndCopies(function);
    }
};

class ScalarLoadForwardingPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return forwardScalarLoads(function);
    }
};

class TailRecursionEliminationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return eliminateTailRecursion(function);
    }
};

class ReadOnlyGlobalLoadHoistingPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return hoistReadOnlyGlobalLoads(function);
    }
};

class EntryConstantPoolingPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return poolEntryConstants(function);
    }
};

class LoopInvariantCodeMotionPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return hoistLoopInvariantCode(function);
    }
};

class InductionStrengthReductionPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return reduceInductionVariableStrength(function);
    }
};

class ClosedFormLoopPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return simplifyClosedFormLoops(function);
    }
};

class ModularArithmeticPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return optimizeModularArithmetic(function);
    }
};

class TerminatorCleanupPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return cleanupAfterTerminators(function);
    }
};

class DeadCodeEliminationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return eliminateDeadCode(function);
    }
};

class ScalarMemoryCleanupPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return cleanupScalarMemory(function);
    }
};

class DeadStoreEliminationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return eliminateDeadScalarStores(function);
    }
};

class UnreachableBlockEliminationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return removeUnreachableBlocks(function);
    }
};

class PhiCleanupPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return simplifyPhiNodes(function);
    }
};

class SparseConditionalConstantPropagationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return sparseConditionalConstantPropagation(function);
    }
};

class CFGSimplificationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return simplifyControlFlow(function);
    }
};

class MemoryValueOptimizationPass final : public FunctionPass {
public:
    bool run(Function &function) override {
        return optimizeMemoryValues(function);
    }
};

class PassManager {
    std::vector<std::unique_ptr<FunctionPass>> passes;

public:
    template<class Pass, class... Args>
    void addPass(Args &&...args) {
        passes.push_back(std::make_unique<Pass>(std::forward<Args>(args)...));
    }

    void run(Function &function) {
        constexpr int maxIterations = 8;
        for (int iteration = 0; iteration < maxIterations; ++iteration) {
            bool changed = false;
            for (auto &pass: passes) {
                changed = pass->run(function) || changed;
            }
            if (!changed) {
                break;
            }
        }
    }
};

std::string flattenedArrayLLVMType(Type elementType, const std::vector<int> &dims) {
    int count = 1;
    for (int dim: dims) {
        count *= dim;
    }
    return "[" + std::to_string(count) + " x " + llvmType(ptrToValue(elementType)) + "]";
}

std::string globalLLVMType(const GlobVar &globVar) {
    if (globVar.dims.empty()) {
        return llvmType(globVar.type);
    }
    return flattenedArrayLLVMType(globVar.type, globVar.dims);
}

std::string globalLLVMInit(const GlobVar &globVar) {
    if (globVar.dims.empty()) {
        int value = globVar.initVal.empty() ? 0 : globVar.initVal.front();
        return std::to_string(normalizeForType(value, globVar.type));
    }

    std::ostringstream out;
    out << "[";
    const std::string elemType = llvmType(ptrToValue(globVar.type));
    for (size_t i = 0; i < globVar.initVal.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << elemType << " " << normalizeForType(globVar.initVal[i], ptrToValue(globVar.type));
    }
    out << "]";
    return out.str();
}

std::string paramsLLVMString(const Params &params) {
    std::ostringstream out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto &param = params[i];
        out << (param.dims.empty() ? llvmType(param.type) : "ptr")
            << " %" << param.name;
    }
    return out.str();
}

std::string combinedConditionalBranch(const Inst &branch, const Inst &fallthrough) {
    if ((branch.op != Op::Bif0 && branch.op != Op::Bif1) || fallthrough.op != Op::Br) {
        return {};
    }
    const auto *taken = asLabel(branch.arg2);
    const auto *other = asLabel(fallthrough.arg1);
    if (!taken || !other) {
        return {};
    }

    const Label *trueLabel = branch.op == Op::Bif1 ? taken : other;
    const Label *falseLabel = branch.op == Op::Bif1 ? other : taken;
    return "br i1 " + valueLLVMName(branch.arg1.get())
           + ", label " + labelLLVMName(*trueLabel)
           + ", label " + labelLLVMName(*falseLabel);
}

void outputFunctionIR(const Function &function) {
    writeIRLine("define " + llvmType(function.getReturnType())
                + " @" + function.getName()
                + "(" + paramsLLVMString(function.getParams()) + ") {");
    for (const auto &block: function.getBasicBlocks()) {
        block->outputIR();
    }
    writeIRLine("}");
    writeIRLine("");
}
} // namespace

Element::Element(ValueKind valueKind, Type valueType) :
    valueKind(valueKind),
    valueType(valueType) {}

ValueKind Element::getKind() const {
    return valueKind;
}

Type Element::getValueType() const {
    return valueType;
}

bool Element::isSameValue(const Element &other) const {
    return valueKey() == other.valueKey();
}

void IR::reset() {
    Str::MIPS_strings.clear();
    Str::idAllocator = 0;
    Label::idAllocator = 0;
}

Module::Module(std::string name) :
    name(std::move(name)) {}

const std::vector<std::unique_ptr<Function>> &Module::getFunctions() const {
    return functions;
}

std::vector<std::unique_ptr<Function>> &Module::getMutableFunctions() {
    return functions;
}

Inst::Inst(Op op,
           std::unique_ptr<Element> res,
           std::unique_ptr<Element> arg1,
           std::unique_ptr<Element> arg2) :
    op(op),
    res(std::move(res)),
    arg1(std::move(arg1)),
    arg2(std::move(arg2)) {}

PhiIncoming::PhiIncoming(std::string predecessor, std::unique_ptr<Element> value) :
    predecessor(std::move(predecessor)),
    value(std::move(value)) {}

void Inst::outputIR() const {
    const auto line = toLLVMString();
    if (!line.empty()) {
        writeIRLine("  " + line);
    }
}

std::string Inst::toString() const {
    std::string result = opToStr(op) + '\t'
                         + (res ? res->toString() : "_") + '\t'
                         + (arg1 ? arg1->toString() : "_") + '\t'
                         + (arg2 ? arg2->toString() : "_");
    for (const auto &incoming: phiIncoming) {
        result += "\t[" + (incoming.value ? incoming.value->toString() : "_")
                  + ", " + incoming.predecessor + "]";
    }
    return result;
}

std::vector<OperandRef> Inst::operands() const {
    std::vector<OperandRef> result;
    result.reserve(3 + phiIncoming.size());
    auto add = [&](OperandRole role, size_t slot, const std::unique_ptr<Element> &value) {
        if (value) {
            result.push_back({role, slot, value.get()});
        }
    };

    switch (op) {
        case Op::Alloca:
            add(OperandRole::Definition, 1, arg1);
            add(OperandRole::Size, 2, arg2);
            break;
        case Op::Store:
        case Op::StoreDynamic:
            add(OperandRole::Value, 0, res);
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Offset, 2, arg2);
            break;
        case Op::MemZero:
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Size, 2, arg2);
            break;
        case Op::Load:
        case Op::LoadPtr:
        case Op::LoadDynamic:
            add(OperandRole::Definition, 0, res);
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Offset, 2, arg2);
            break;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::XorLimb:
        case Op::AndLimb:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::MulImd:
            add(OperandRole::Definition, 0, res);
            add(OperandRole::Value, 1, arg1);
            add(OperandRole::Value, 2, arg2);
            break;
        case Op::LoadImd:
            add(OperandRole::Definition, 0, res);
            add(OperandRole::Value, 1, arg1);
            break;
        case Op::Neg:
        case Op::Mult4:
        case Op::NewMove:
        case Op::Not:
            add(OperandRole::Definition, 0, res);
            add(OperandRole::Value, 1, arg1);
            break;
        case Op::Phi:
            add(OperandRole::Definition, 0, res);
            for (size_t index = 0; index < phiIncoming.size(); ++index) {
                add(OperandRole::Value, 3 + index, phiIncoming[index].value);
            }
            break;
        case Op::Parameter:
            add(OperandRole::Definition, 0, res);
            add(OperandRole::Address, 1, arg1);
            break;
        case Op::GetString:
            add(OperandRole::Value, 0, res);
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Offset, 2, arg2);
            break;
        case Op::PrintInt:
        case Op::PrintChar:
            add(OperandRole::Value, 1, arg1);
            break;
        case Op::PrintStr:
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Offset, 2, arg2);
            break;
        case Op::Br:
            add(OperandRole::Target, 1, arg1);
            break;
        case Op::Bif0:
        case Op::Bif1:
            add(OperandRole::Value, 1, arg1);
            add(OperandRole::Target, 2, arg2);
            break;
        case Op::Call:
            add(OperandRole::Callee, 1, arg1);
            break;
        case Op::Ret:
        case Op::RetMain:
            add(OperandRole::Value, 1, arg1);
            break;
        case Op::PushParam:
            add(OperandRole::Value, 1, arg1);
            break;
        case Op::PushAddressParam:
            add(OperandRole::Address, 1, arg1);
            add(OperandRole::Offset, 2, arg2);
            break;
        default:
            break;
    }
    return result;
}

std::unique_ptr<Element> &Inst::operandSlot(size_t slot) {
    switch (slot) {
        case 0:
            return res;
        case 1:
            return arg1;
        case 2:
            return arg2;
        default:
            if (op == Op::Phi && slot >= 3 && slot - 3 < phiIncoming.size()) {
                return phiIncoming[slot - 3].value;
            }
            Error::raise("Bad IR operand slot");
            return res;
    }
}

const std::unique_ptr<Element> &Inst::operandSlot(size_t slot) const {
    switch (slot) {
        case 0:
            return res;
        case 1:
            return arg1;
        case 2:
            return arg2;
        default:
            if (op == Op::Phi && slot >= 3 && slot - 3 < phiIncoming.size()) {
                return phiIncoming[slot - 3].value;
            }
            Error::raise("Bad IR operand slot");
            return res;
    }
}

void Inst::setOperand(size_t slot, std::unique_ptr<Element> value) {
    operandSlot(slot) = std::move(value);
}

void Inst::addPhiIncoming(std::string predecessor, std::unique_ptr<Element> value) {
    phiIncoming.emplace_back(std::move(predecessor), std::move(value));
}

std::string Inst::toLLVMString() const {
    const std::string resName = valueLLVMName(res.get());
    const std::string arg1Name = valueLLVMName(arg1.get());
    const std::string arg2Name = valueLLVMName(arg2.get());
    const std::string resType = valueLLVMType(elementType(res.get()));
    const std::string arg1Type = valueLLVMType(elementType(arg1.get()));

    switch (op) {
        case Op::Empty:
            return "";
        case Op::InStack:
            return "; scope.in";
        case Op::OutStack:
            return "; scope.out";
        case Op::Alloca:
            if (auto var = dynamic_cast<const Var *>(arg1.get())) {
                auto size = dynamic_cast<const ConstVal *>(arg2.get());
                if (!var->dims.empty() || (size && size->value > 1)) {
                    std::vector<int> dims = var->dims;
                    if (dims.empty() && size) {
                        dims.push_back(size->value);
                    }
                    return varLLVMName(*var) + " = alloca " + flattenedArrayLLVMType(var->type, dims);
                }
                return varLLVMName(*var) + " = alloca " + llvmType(ptrToValue(var->type));
            }
            return "alloca";
        case Op::Store:
        case Op::StoreDynamic:
            return "store " + valueLLVMType(elementType(res.get())) + " " + resName
                   + ", ptr " + arg1Name
                   + (arg2 ? ", i32 " + arg2Name : "");
        case Op::MemZero:
            return "call void @llvm.memset.zero(ptr " + arg1Name
                   + ", i32 " + arg2Name + ")";
        case Op::Load:
        case Op::LoadDynamic:
            return resName + " = load " + resType
                   + ", ptr " + arg1Name
                   + (arg2 ? ", i32 " + arg2Name : "");
        case Op::LoadPtr:
            return resName + " = load " + resType
                   + ", ptr " + arg1Name
                   + (arg2 ? ", i32 " + arg2Name : "");
        case Op::Add:
            return resName + " = add " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Sub:
            return resName + " = sub " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Mul:
            return resName + " = mul " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Div:
            return resName + " = sdiv " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Mod:
            return resName + " = srem " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::And:
            return resName + " = and " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Or:
            return resName + " = or " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Xor:
            return resName + " = xor " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::XorLimb:
            return resName + " = xor.limb " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::AndLimb:
            return resName + " = and.limb " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Leq:
            return resName + " = icmp sle " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::Lss:
            return resName + " = icmp slt " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::Geq:
            return resName + " = icmp sge " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::Gre:
            return resName + " = icmp sgt " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::Eql:
            return resName + " = icmp eq " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::Neq:
            return resName + " = icmp ne " + arg1Type + " " + arg1Name + ", " + arg2Name;
        case Op::LoadImd:
            return resName + " = const " + resType + " " + arg1Name;
        case Op::MulImd:
            return resName + " = mul " + resType + " " + arg1Name + ", " + arg2Name;
        case Op::Neg:
            return resName + " = sub " + resType + " 0, " + arg1Name;
        case Op::Mult4:
            return resName + " = shl " + resType + " " + arg1Name + ", 2";
        case Op::NewMove:
            return resName + " = mov " + resType + " " + arg1Name;
        case Op::Phi: {
            std::string result = resName + " = phi " + resType + " ";
            for (size_t index = 0; index < phiIncoming.size(); ++index) {
                if (index != 0) {
                    result += ", ";
                }
                result += "[ " + valueLLVMName(phiIncoming[index].value.get())
                          + ", %" + phiIncoming[index].predecessor + " ]";
            }
            return result;
        }
        case Op::Parameter:
            return resName + " = param " + resType + " " + arg1Name;
        case Op::Not:
            return resName + " = icmp eq " + arg1Type + " " + arg1Name + ", 0";
        case Op::GetInt:
            return "call i32 @get_int() ; result in $v0";
        case Op::GetChar:
            return "call i8 @get_char() ; result in $v0";
        case Op::GetString:
            return "call void @get_string(ptr " + arg1Name + ", i32 " + resName
                   + (arg2 ? ", i32 " + arg2Name : "") + ")";
        case Op::PrintInt:
            return "call void @put_int(i32 " + arg1Name + ")";
        case Op::PrintChar:
            return "call void @put_char(i8 " + arg1Name + ")";
        case Op::PrintStr:
            return "call void @put_string(ptr " + arg1Name
                   + (arg2 ? ", i32 " + arg2Name : "") + ")";
        case Op::Br:
            return "br label " + arg1Name;
        case Op::Bif0:
            return "br i1 " + arg1Name + ", label %fallthrough, label " + arg2Name;
        case Op::Bif1:
            return "br i1 " + arg1Name + ", label " + arg2Name + ", label %fallthrough";
        case Op::Call:
            return "call void @" + (arg1 ? arg1->toString() : "");
        case Op::Ret:
            return arg1 ? "ret " + arg1Type + " " + arg1Name : "ret void";
        case Op::RetMain:
            return arg1 ? "ret i32 " + arg1Name : "ret i32 0";
        case Op::PushParam:
            return "param " + arg1Type + " " + arg1Name;
        case Op::PushAddressParam:
            return "param ptr " + arg1Name + (arg2 ? ", i32 " + arg2Name : "");
        default:
            Error::raise("Bad IR op");
            return "";
    }
}

bool Inst::definesTemp() const {
    auto temp = asTemp(res);
    if (!temp || temp->id < 0) {
        return false;
    }
    switch (op) {
        case Op::Load:
        case Op::LoadPtr:
        case Op::LoadDynamic:
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::XorLimb:
        case Op::AndLimb:
        case Op::Leq:
        case Op::Lss:
        case Op::Geq:
        case Op::Gre:
        case Op::Eql:
        case Op::Neq:
        case Op::LoadImd:
        case Op::MulImd:
        case Op::Neg:
        case Op::Mult4:
        case Op::NewMove:
        case Op::Phi:
        case Op::Parameter:
        case Op::Not:
            return true;
        default:
            return false;
    }
}

bool Inst::definesValue() const {
    switch (op) {
        case Op::Alloca:
            return arg1 != nullptr;
        default:
            return definesTemp();
    }
}

bool Inst::isScopeMarker() const {
    return op == Op::InStack || op == Op::OutStack;
}

bool Inst::isTerminator() const {
    return op == Op::Br || op == Op::Bif0 || op == Op::Bif1 || op == Op::Ret || op == Op::RetMain;
}

bool Inst::isUnconditionalTerminator() const {
    return op == Op::Br || op == Op::Ret || op == Op::RetMain;
}

bool Inst::mayHaveSideEffects() const {
    switch (op) {
        case Op::InStack:
        case Op::OutStack:
        case Op::Alloca:
        case Op::Store:
        case Op::StoreDynamic:
        case Op::MemZero:
        case Op::GetInt:
        case Op::GetChar:
        case Op::GetString:
        case Op::PrintInt:
        case Op::PrintChar:
        case Op::PrintStr:
        case Op::Br:
        case Op::Bif0:
        case Op::Bif1:
        case Op::Call:
        case Op::Ret:
        case Op::RetMain:
        case Op::PushParam:
        case Op::PushAddressParam:
            return true;
        default:
            return false;
    }
}

bool Inst::isRemovableIfUnused() const {
    return definesTemp() && !mayHaveSideEffects();
}

std::string Inst::opToStr(Op anOperator) {
    switch (anOperator) {
        case Op::Store:
            return "Store";
        case Op::StoreDynamic:
            return "StoreDynamic";
        case Op::MemZero:
            return "MemZero";
        case Op::Add:
            return "Add";
        case Op::Sub:
            return "Sub";
        case Op::Mul:
            return "Mul";
        case Op::Div:
            return "Div";
        case Op::Mod:
            return "Mod";
        case Op::And:
            return "And";
        case Op::Or:
            return "Or";
        case Op::Xor:
            return "Xor";
        case Op::XorLimb:
            return "XorLimb";
        case Op::AndLimb:
            return "AndLimb";
        case Op::Neg:
            return "Neg";
        case Op::LoadImd:
            return "LoadImd";
        case Op::GetInt:
            return "GetInt";
        case Op::GetChar:
            return "GetChar";
        case Op::GetString:
            return "GetString";
        case Op::PrintInt:
            return "PrintInt";
        case Op::PrintChar:
            return "PrintChar";
        case Op::PrintStr:
            return "PrintStr";
        case Op::Alloca:
            return "Alloca";
        case Op::Load:
            return "Load";
        case Op::LoadPtr:
            return "LoadPtr";
        case Op::LoadDynamic:
            return "LoadDynamic";
        case Op::Br:
            return "Br";
        case Op::Bif0:
            return "Bif0";
        case Op::Call:
            return "Call";
        case Op::PushParam:
            return "PushParam";
        case Op::Ret:
            return "Ret";
        case Op::RetMain:
            return "RetMain";
        case Op::InStack:
            return "InStack";
        case Op::OutStack:
            return "OutStack";
        case Op::NewMove:
            return "NewMove";
        case Op::Phi:
            return "Phi";
        case Op::Parameter:
            return "Parameter";
        case Op::Leq:
            return "Leq";
        case Op::Lss:
            return "Lss";
        case Op::Geq:
            return "Geq";
        case Op::Gre:
            return "Gre";
        case Op::Eql:
            return "Eql";
        case Op::Neq:
            return "Neq";
        case Op::Bif1:
            return "Bif1";
        case Op::MulImd:
            return "MulImd";
        case Op::Mult4:
            return "Mult4";
        case Op::PushAddressParam:
            return "PushAddressParam";
        case Op::Not:
            return "Not";
        case Op::Empty:
            return "Empty";
        default:
            Error::raise("Bad IR op");
            return "Bad IR op";
    }
}

Label::Label(std::string name, bool isFunc) :
    Element(ValueKind::Label, Type::Void) {
    if (isFunc) {
        this->nameAndId = std::move(name);
    } else {
        this->nameAndId = std::move(name) + "_" + std::to_string(idAllocator++);
    }
}

std::string Label::toString() const {
    return nameAndId;
}

std::unique_ptr<Element> Label::clone() const {
    return std::make_unique<Label>(*this);
}

std::string Label::valueKey() const {
    return "label:" + nameAndId;
}

GlobVar::GlobVar(
        bool cons,
        Type type,
        std::vector<int> dims,
        std::vector<int> initVal) :
    cons(cons),
    type(type),
    dims(std::move(dims)),
    initVal(std::move(initVal)) {}

GlobVar::GlobVar(bool cons, Type type, const std::vector<int> &dims) :
    cons(cons),
    type(type),
    dims(dims) {
    int size = 1;
    for (const int i: dims) {
        size *= i;
    }
    initVal = std::vector<int>(size, 0);
}

Function::Function(std::string name,
                   Type returnType,
                   Params params) :
    name(std::move(name)),
    returnType(returnType),
    params(std::move(params)) {
    idAllocator = 0;
}

void Function::moveBasicBlocks(BasicBlocks &&bBlocks) {
    basicBlocks = std::move(bBlocks);
}

const BasicBlocks &Function::getBasicBlocks() const {
    return basicBlocks;
}

BasicBlocks &Function::getMutableBasicBlocks() {
    return basicBlocks;
}

const std::string &Function::getName() const {
    return name;
}

Type Function::getReturnType() const {
    return returnType;
}

const Params &Function::getParams() const {
    return params;
}

Function::UseDefChain Function::buildUseDefChains() {
    UseDefChain chains;
    for (auto &block: basicBlocks) {
        for (auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                if (!operand.value) {
                    continue;
                }
                auto &record = chains[operand.value->valueKey()];
                record.key = operand.value->valueKey();
                if (operand.role == OperandRole::Definition) {
                    record.definition = operand.value;
                } else {
                    record.uses.push_back({&inst, operand.slot, operand.role});
                }
            }
        }
    }
    return chains;
}

bool Function::replaceAllUsesWith(const Element &from, const Element &to) {
    bool changed = false;
    auto chains = buildUseDefChains();
    auto iter = chains.find(from.valueKey());
    if (iter == chains.end()) {
        return false;
    }
    for (const auto &use: iter->second.uses) {
        if (use.user) {
            use.user->setOperand(use.slot, to.clone());
            changed = true;
        }
    }
    return changed;
}

void Module::outputIR() const {
#if defined(STDOUT_IR) || defined(FILEOUT_IR)
    writeIRLine("; ModuleID = '" + name + "'");
    writeIRLine("source_filename = \"" + name + "\"");
    writeIRLine("");
    for (const auto &entry: globVars) {
        const auto &ident = entry.first;
        const auto &globVar = entry.second;
        writeIRLine("@" + ident + " = " + (globVar.cons ? "constant " : "global ")
                    + globalLLVMType(globVar) + " " + globalLLVMInit(globVar));
    }
    if (!globVars.empty()) {
        writeIRLine("");
    }
    for (size_t i = 0; i < Str::MIPS_strings.size(); ++i) {
        writeIRLine("@str_" + std::to_string(i) + " = private unnamed_addr constant c"
                    + Str::MIPS_strings[i]);
    }
    if (!Str::MIPS_strings.empty()) {
        writeIRLine("");
    }
    if (mainFunction) {
        outputFunctionIR(*mainFunction);
    }
    for (auto &function: functions) {
        outputFunctionIR(*function);
    }
#endif
}

void Module::optimize() {
    if (mainFunction) {
        normalizeBasicBlocks(*mainFunction);
    }
    for (auto &function: functions) {
        normalizeBasicBlocks(*function);
    }

    for (int iteration = 0; iteration < 32 && inlineFunctions(*this); ++iteration) {
    }
    eliminateUnreachableFunctions(*this);
    const auto readOnlyGlobals = findReadOnlyGlobals(
            globVars, mainFunction.get(), functions);

    PassManager passManager;
    passManager.addPass<ReadOnlyGlobalLoadHoistingPass>();
    passManager.addPass<SparseConditionalConstantPropagationPass>();
    passManager.addPass<ConstantFoldPass>();
    passManager.addPass<TerminatorCleanupPass>();
    passManager.addPass<CFGSimplificationPass>();
    passManager.addPass<UnreachableBlockEliminationPass>();
    passManager.addPass<PhiCleanupPass>();
    passManager.addPass<ScalarLoadForwardingPass>();
    passManager.addPass<MemoryValueOptimizationPass>();
    passManager.addPass<GlobalPropagationPass>();
    passManager.addPass<EntryConstantPoolingPass>();
    passManager.addPass<InductionStrengthReductionPass>();
    passManager.addPass<LoopInvariantCodeMotionPass>();
    passManager.addPass<ClosedFormLoopPass>();
    passManager.addPass<ModularArithmeticPass>();
    passManager.addPass<DeadStoreEliminationPass>();
    passManager.addPass<DeadCodeEliminationPass>();
    passManager.addPass<ScalarMemoryCleanupPass>();

    auto optimizeFunction = [&](Function &function) {
        propagateConstantGlobals(function, globVars, readOnlyGlobals);
        cleanupAfterTerminators(function);
        removeUnreachableBlocks(function);
        eliminateTailRecursion(function);
        while (canonicalizeLoops(function)) {
        }
        promoteMemoryToRegisters(function);
        passManager.run(function);
        std::string verificationFailure;
        if (!verifySSA(function, &verificationFailure)) {
            Error::raise("SSA verification failed in " + function.getName()
                         + ": " + verificationFailure);
        }
        if (globalValueNumberingCodeMotion(function)) {
            passManager.run(function);
        }
        for (int iteration = 0; iteration < 8
                                && eliminatePartialRedundancy(function);
             ++iteration) {
            globalValueNumberingCodeMotion(function);
            passManager.run(function);
        }
        if (!verifySSA(function, &verificationFailure)) {
            Error::raise("SSA verification failed after PRE in " + function.getName()
                         + ": " + verificationFailure);
        }
    };

    if (mainFunction) {
        optimizeFunction(*mainFunction);
    }
    for (auto &function: functions) {
        optimizeFunction(*function);
    }

    for (int iteration = 0; iteration < 16; ++iteration) {
        bool changed = optimizeInterproceduralCalls(*this);
        changed = inlineTrivialFunctions(*this) || changed;
        changed = inlineLinearFunctions(*this) || changed;
        changed = inlineLinearArrayFunctions(*this) || changed;
        changed = specializeConstantArguments(*this) || changed;
        if (!changed) {
            break;
        }
        eliminateUnreachableFunctions(*this);
        if (mainFunction) {
            optimizeFunction(*mainFunction);
        }
        for (auto &function: functions) {
            optimizeFunction(*function);
        }
    }
    eliminateUnreachableFunctions(*this);
}

void Module::lowerPhiNodes() {
    if (mainFunction) {
        IR::lowerPhiNodes(*mainFunction);
    }
    for (auto &function: functions) {
        IR::lowerPhiNodes(*function);
    }
}

const std::vector<std::pair<std::string, GlobVar>> &Module::getGlobVars() const {
    return globVars;
}

const Function &Module::getMainFunction() const {
    return *mainFunction;
}

Function &Module::getMutableMainFunction() {
    return *mainFunction;
}

void Module::setMainFunction(std::unique_ptr<Function> main_function) {
    mainFunction = std::move(main_function);
}

void Module::addFunction(std::unique_ptr<Function> function) {
    functions.emplace_back(std::move(function));
}

void Module::addGlobVar(std::string name, GlobVar globVar) {
    globVars.emplace_back(std::move(name), std::move(globVar));
}

void BasicBlock::addInst(Inst inst) {
    instructions.push_back(std::move(inst));
}

bool BasicBlock::hasInstructions() const {
    return !instructions.empty();
}

BasicBlock::BasicBlock(std::string labelName, bool isFunc) :
    label(Label(std::move(labelName), isFunc)) {}

void BasicBlock::outputIR() const {
    writeIRLine(label.nameAndId + ":");
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (i + 1 < instructions.size()) {
            auto combined = combinedConditionalBranch(instructions[i], instructions[i + 1]);
            if (!combined.empty()) {
                writeIRLine("  " + combined);
                ++i;
                continue;
            }
        }
        const auto line = instructions[i].toLLVMString();
        if (!line.empty()) {
            writeIRLine("  " + line);
        }
    }
}

Var::Var(std::string name,
         int depth,
         bool cons,
         const std::vector<int> &dims,
         Type type,
         bool storesAddress) :
    Element(ValueKind::Variable, type),
    name(std::move(name)),
    depth(depth),
    cons(cons),
    dims(dims),
    type(type),
    storesAddress(storesAddress) {}

Var::Var(std::string name, int depth) :
    Element(ValueKind::Variable, Type::Int),
    name(std::move(name)),
    depth(depth),
    cons(false),
    type(Type::Int),
    storesAddress(false) {}

bool IR::operator==(const Var &lhs, const Var &rhs) {
    return lhs.name == rhs.name
           && lhs.depth == rhs.depth;
}

std::size_t IR::hash_value(const Var &obj) {
    std::size_t seed = 0x2100EB1C;
    seed ^= (seed << 6) + (seed >> 2) + 0x0CCF5A17 + std::hash<std::string>()(obj.name);
    seed ^= (seed << 6) + (seed >> 2) + 0x6CBE80FE + static_cast<std::size_t>(obj.depth);
    return seed;
}

bool Var::operator<(const Var &other) const {
    return name + std::to_string(depth) < other.name + std::to_string(other.depth);
}

std::string Var::toString() const {
    std::string s = name + "(" + std::to_string(depth) + ")";
    for (auto &i: dims) {
        s += "[" + std::to_string(i) + "]";
    }
    return s;
}

std::unique_ptr<Element> Var::clone() const {
    return std::make_unique<Var>(*this);
}

std::string Var::valueKey() const {
    return "var:" + name + ":" + std::to_string(depth);
}

Temp::Temp(Type type) :
    Element(ValueKind::Temporary, type),
    type(type) {
    id = Function::idAllocator++;
}

Temp::Temp(int id, Type type) :
    Element(ValueKind::Temporary, type),
    id(id),
    type(type) {}

std::string Temp::toString() const {
    return "%" + std::to_string(id);
}

Temp::Temp(const Temp &other) :
    Element(other.valueKind, other.valueType) {
    id = other.id;
    type = other.type;
}

std::unique_ptr<Element> Temp::clone() const {
    return std::make_unique<Temp>(*this);
}

std::string Temp::valueKey() const {
    return "tmp:" + std::to_string(id);
}

ConstVal::ConstVal(int value, Type type) :
    Element(ValueKind::Constant, type),
    value(value),
    type(type) {}

std::string ConstVal::toString() const {
    return std::to_string(value);
}

std::unique_ptr<Element> ConstVal::clone() const {
    return std::make_unique<ConstVal>(*this);
}

std::string ConstVal::valueKey() const {
    return "const:" + typeKey(type) + ":" + std::to_string(value);
}

Str::Str() :
    Element(ValueKind::String, Type::CharPtr) {
    id = idAllocator++;
}

std::string Str::toString() const {
    return "str_" + std::to_string(id);
}

std::unique_ptr<Element> Str::clone() const {
    return std::make_unique<Str>(*this);
}

std::string Str::valueKey() const {
    return "str:" + std::to_string(id);
}
