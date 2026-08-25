//
// Created by Steel_Shadow on 2023/10/31.
//
#include "MIPS.h"

#include "config.h"
#include "errorHandler/Error.h"
#include "Instruction.h"
#include "Memory.h"
#include "middle/Analysis.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace MIPS;

int MIPS::curDepth = 1;

std::ofstream MIPS::mipsFileStream;
std::vector<std::unique_ptr<Assembly>> MIPS::assemblies; // maybe use List is faster in optimization

void MIPS::reset() {
    curDepth = 1;
    assemblies.clear();
    clearRegs();
    StackMemory::varToOffset.clear();
    StackMemory::tempToOffset.clear();
    StackMemory::curOffset = 0;
    StackMemory::offsetStack = std::stack<int>{};
}

void MIPS::output(const std::string &str, bool newLine) {
#if defined(FILEOUT_MIPS)
    mipsFileStream << str;
    if (newLine) {
        mipsFileStream << '\n';
    }
#endif
#if defined(STDOUT_MIPS)
    std::cout << str;
    if (newLine) {
        std::cout << '\n';
    }
#endif
}

Label::Label(std::string name_and_id) :
    nameAndId(std::move(name_and_id)) {}

Label::Label(const IR::Label *label) :
    nameAndId(label->nameAndId) {}

std::string Label::toString() {
    return nameAndId + ":";
}

namespace {
bool simplifyConditionalBranches();
bool eliminateRedundantInstructions();
bool batchRepeatedHalving();
bool batchOddSuccessorHalving();
bool memoizeBoundedTailRecurrences(const IR::Module &module);

bool isComparison(IR::Op op) {
    return op == IR::Op::Leq || op == IR::Op::Lss || op == IR::Op::Geq
           || op == IR::Op::Gre || op == IR::Op::Eql || op == IR::Op::Neq;
}

Op comparisonBranch(IR::Op comparison, IR::Op branch) {
    const bool branchOnTrue = branch == IR::Op::Bif1;
    switch (comparison) {
        case IR::Op::Eql:
            return branchOnTrue ? Op::beq : Op::bne;
        case IR::Op::Neq:
            return branchOnTrue ? Op::bne : Op::beq;
        case IR::Op::Lss:
            return branchOnTrue ? Op::blt : Op::bge;
        case IR::Op::Leq:
            return branchOnTrue ? Op::ble : Op::bgt;
        case IR::Op::Gre:
            return branchOnTrue ? Op::bgt : Op::ble;
        case IR::Op::Geq:
            return branchOnTrue ? Op::bge : Op::blt;
        default:
            return Op::none;
    }
}

class CodeGenerator {
    const IR::Module &module;
    std::unordered_map<std::string, size_t> registerArgumentFunctions;

public:
    explicit CodeGenerator(const IR::Module &module) :
        module(module) {
        configureRegisterArgumentConvention(module);
        for (const auto &function: module.getFunctions()) {
            if (usesRegisterArguments(*function)) {
                registerArgumentFunctions.emplace(
                        function->getName(), function->getParams().size());
            }
        }
    }

    void run() {
        MIPS::reset();
        emitDataSegment();
        emitTextSegment();
        optimizeText();
        outputText();
    }

private:
    void emitDataSegment() const {
        MIPS::output("#### MIPS ####");
        MIPS::output(".data");
        for (auto &[name, globVar]: module.getGlobVars()) {
            std::string dataLine = name + (globVar.type == Type::Char ? ": .byte " : ": .word ");
            for (size_t i = 0; i < globVar.initVal.size(); ++i) {
                if (i != 0) {
                    dataLine += ", ";
                }
                dataLine += std::to_string(globVar.initVal[i]);
            }
            MIPS::output(dataLine);
        }
        int i = 0;
        for (const auto &str: IR::Str::MIPS_strings) {
            MIPS::output("str_" + std::to_string(i) + ": .asciiz " + str);
            i++;
        }
    }

    void emitTextSegment() {
        MIPS::output("");
        MIPS::output(".text");
        prepareFunction(module.getMainFunction(), true);
        emitFunction(module.getMainFunction(), true);
        for (auto &func: module.getFunctions()) {
            prepareFunction(*func, false);
            emitFunction(*func, false);
        }
    }

    std::unordered_map<size_t, Register> registerArgumentsForBlock(
            const IR::BasicBlock &block) const {
        std::unordered_map<size_t, Register> result;
        const auto &instructions = block.instructions;
        for (size_t call = 0; call < instructions.size(); ++call) {
            const auto *callee = dynamic_cast<const IR::Label *>(instructions[call].arg1.get());
            auto function = callee ? registerArgumentFunctions.find(callee->nameAndId)
                                   : registerArgumentFunctions.end();
            if (instructions[call].op != IR::Op::Call
                || function == registerArgumentFunctions.end()) {
                continue;
            }

            size_t frameStart = call;
            size_t nestedFrames = 0;
            for (size_t reverse = call; reverse-- > 0;) {
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
            if (frameStart == call) {
                continue;
            }

            std::vector<size_t> pushes;
            bool nestedCall = false;
            for (size_t index = frameStart + 1; index < call; ++index) {
                nestedCall = nestedCall || instructions[index].op == IR::Op::Call;
                if (instructions[index].op == IR::Op::PushParam) {
                    pushes.push_back(index);
                } else if (instructions[index].op == IR::Op::PushAddressParam) {
                    pushes.clear();
                    nestedCall = true;
                    break;
                }
            }
            if (nestedCall || pushes.size() != function->second) {
                continue;
            }
            for (size_t index = 0; index < pushes.size(); ++index) {
                result.emplace(pushes[index], argumentRegister(pushes.size() - index - 1));
            }
        }
        return result;
    }

    void emitFunction(const IR::Function &function, bool isMain) const {
        std::unordered_map<int, int> useCounts;
        for (const auto &block: function.getBasicBlocks()) {
            for (const auto &inst: block->instructions) {
                for (int temp: IR::usedTemps(inst)) {
                    ++useCounts[temp];
                }
            }
        }

        const size_t functionAssemblyStart = assemblies.size();
        std::vector<std::pair<size_t, size_t>> blockRanges;
        blockRanges.reserve(function.getBasicBlocks().size());
        bool firstBlock = true;
        for (size_t blockIndex = 0;
             blockIndex < function.getBasicBlocks().size();
             ++blockIndex) {
            const auto &basicBlock = function.getBasicBlocks()[blockIndex];
            const size_t blockStart = assemblies.size();
            const auto registerArguments = registerArgumentsForBlock(*basicBlock);
            assemblies.push_back(std::make_unique<Label>(basicBlock->label.nameAndId));
            if (firstBlock) {
                emitFunctionPrologue(function, isMain);
                firstBlock = false;
            }
            for (size_t index = 0; index < basicBlock->instructions.size(); ++index) {
                const auto &inst = basicBlock->instructions[index];
                auto argument = registerArguments.find(index);
                if (argument != registerArguments.end()) {
                    const auto *value = dynamic_cast<const IR::Temp *>(inst.arg1.get());
                    if (value) {
                        MIPS::beginInstruction(inst);
                        if (auto constant = MIPS::knownConstant(inst.arg1.get())) {
                            assemblies.push_back(std::make_unique<I_imm_Inst>(
                                    Op::li, argument->second, Register::none, *constant));
                        } else {
                            assemblies.push_back(std::make_unique<R_Inst>(
                                    Op::move, argument->second, MIPS::getReg(value), Register::none));
                        }
                        MIPS::endInstruction();
                        continue;
                    }
                }
                if (index + 1 < basicBlock->instructions.size() && isComparison(inst.op)) {
                    const auto &branch = basicBlock->instructions[index + 1];
                    const auto *result = dynamic_cast<const IR::Temp *>(inst.res.get());
                    const auto *condition = dynamic_cast<const IR::Temp *>(branch.arg1.get());
                    const auto *lhs = dynamic_cast<const IR::Temp *>(inst.arg1.get());
                    const auto *rhs = dynamic_cast<const IR::Temp *>(inst.arg2.get());
                    const auto *target = dynamic_cast<const IR::Label *>(branch.arg2.get());
                    if ((branch.op == IR::Op::Bif0 || branch.op == IR::Op::Bif1)
                        && result && condition && result->id == condition->id
                        && useCounts[result->id] == 1 && lhs && rhs && target) {
                        MIPS::beginInstruction(inst);
                        const Op branchOp = comparisonBranch(inst.op, branch.op);
                        const auto lhsConstant = MIPS::knownConstant(inst.arg1.get());
                        const auto rhsConstant = MIPS::knownConstant(inst.arg2.get());
                        if ((branchOp == Op::beq || branchOp == Op::bne)
                            && rhsConstant && *rhsConstant == 0) {
                            assemblies.push_back(std::make_unique<I_label_Inst>(
                                    branchOp == Op::beq ? Op::beqz : Op::bne,
                                    MIPS::getReg(lhs),
                                    branchOp == Op::beq ? Register::none : Register::zero,
                                    Label(target)));
                        } else if ((branchOp == Op::beq || branchOp == Op::bne)
                                   && lhsConstant && *lhsConstant == 0) {
                            assemblies.push_back(std::make_unique<I_label_Inst>(
                                    branchOp == Op::beq ? Op::beqz : Op::bne,
                                    MIPS::getReg(rhs),
                                    branchOp == Op::beq ? Register::none : Register::zero,
                                    Label(target)));
                        } else {
                            assemblies.push_back(std::make_unique<I_label_Inst>(
                                    branchOp,
                                    MIPS::getReg(lhs),
                                    MIPS::getReg(rhs),
                                    Label(target)));
                        }
                        MIPS::endInstruction();
                        ++index;
                        continue;
                    }
                }
                MIPS::irToMips(inst);
            }
            const bool hasHardTerminator = std::any_of(
                    basicBlock->instructions.begin(),
                    basicBlock->instructions.end(),
                    [](const IR::Inst &inst) {
                        return inst.op == IR::Op::Br || inst.op == IR::Op::Ret
                               || inst.op == IR::Op::RetMain;
                    });
            if (!hasHardTerminator
                && blockIndex + 1 < function.getBasicBlocks().size()) {
                assemblies.push_back(std::make_unique<J_Inst>(
                        Op::j,
                        Label(function.getBasicBlocks()[blockIndex + 1]->label.nameAndId)));
            }
            blockRanges.emplace_back(blockStart, assemblies.size());
        }

        if (blockRanges.size() < 3) {
            return;
        }
        const auto cfg = IR::buildControlFlowGraph(function);
        const auto loopDepth = IR::computeLoopDepths(cfg);
        std::vector<bool> placed(blockRanges.size(), false);
        std::vector<size_t> order;
        order.reserve(blockRanges.size());
        auto appendTrace = [&](size_t start) {
            size_t block = start;
            while (!placed[block]) {
                placed[block] = true;
                order.push_back(block);
                size_t best = IR::ControlFlowGraph::NoBlock;
                for (size_t successor: cfg.successors[block]) {
                    if (placed[successor]) {
                        continue;
                    }
                    if (best == IR::ControlFlowGraph::NoBlock
                        || loopDepth[successor] > loopDepth[best]
                        || (loopDepth[successor] == loopDepth[best]
                            && successor < best)) {
                        best = successor;
                    }
                }
                if (best == IR::ControlFlowGraph::NoBlock) {
                    break;
                }
                block = best;
            }
        };
        appendTrace(0);
        while (order.size() < blockRanges.size()) {
            size_t best = IR::ControlFlowGraph::NoBlock;
            for (size_t block = 0; block < blockRanges.size(); ++block) {
                if (!placed[block]
                    && (best == IR::ControlFlowGraph::NoBlock
                        || loopDepth[block] > loopDepth[best])) {
                    best = block;
                }
            }
            appendTrace(best);
        }

        bool reordered = false;
        for (size_t index = 0; index < order.size(); ++index) {
            reordered = reordered || order[index] != index;
        }
        if (!reordered) {
            return;
        }
        std::vector<std::unique_ptr<Assembly>> laidOut;
        laidOut.reserve(assemblies.size() - functionAssemblyStart);
        for (size_t block: order) {
            const auto [begin, end] = blockRanges[block];
            for (size_t assembly = begin; assembly < end; ++assembly) {
                laidOut.push_back(std::move(assemblies[assembly]));
            }
        }
        assemblies.erase(assemblies.begin() + static_cast<long>(functionAssemblyStart),
                         assemblies.end());
        assemblies.insert(assemblies.end(),
                          std::make_move_iterator(laidOut.begin()),
                          std::make_move_iterator(laidOut.end()));
    }

    static void prepareFunction(const IR::Function &func, bool isMain) {
        clearRegs();
        curDepth = 1;
        StackMemory::curOffset = isMain ? 0 : wordSize * CALL_FRAME_RESERVED_WORDS;
        StackMemory::varToOffset.clear();
        StackMemory::tempToOffset.clear();
        StackMemory::offsetStack = std::stack<int>{};

        int offset = 0;
        for (const auto &param: func.getParams()) {
            StackMemory::varToOffset.emplace(IR::Var(param.name, 1, false, param.dims, param.type, !param.dims.empty()), -offset);
            offset += wordSize;
        }

        prepareRegisterAllocation(func);
        reserveSpillSlots();
    }

    void optimizeText() const {
        while (simplifyConditionalBranches()) {}
        while (eliminateRedundantInstructions()) {}
        while (allMergeR_Move()) {}
        while (allMergeMove_R_rs()) {}
        while (allMergeLi_Move()) {}
        while (allMergeMove_R_rt()) {}
        while (allMergeLi_R()) {}
        while (eliminateRedundantInstructions()) {}
        while (batchRepeatedHalving()) {}
        while (batchOddSuccessorHalving()) {}
        memoizeBoundedTailRecurrences(module);
        while (allMergeR_Move()) {}
        while (allMergeMove_R_rs()) {}
        while (allMergeMove_R_rt()) {}
        while (allMergeLi_Move()) {}
        while (allMergeLi_R()) {}
        while (eliminateRedundantInstructions()) {}
    }

    static void outputText() {
        for (auto &assem: assemblies) {
            MIPS::output(assem->toString());
        }
    }
};
} // namespace

void MIPS::genMIPS(const IR::Module &module) {
    CodeGenerator(module).run();
}

Op rOp_ImmOp(Op rOp) {
    switch (rOp) {
        case Op::addu:
            return Op::addiu;
        case Op::subu:
            return Op::subiu;
        case Op::and_:
            return Op::andi;
        case Op::or_:
            return Op::ori;
        case Op::add:
            return Op::addi;
        case Op::slt:
            return Op::slti;
        default:
            return Op::none;
    }
}

std::unique_ptr<I_imm_Inst> MIPS::mergeLi_R(const I_imm_Inst &li, const R_Inst &r) {
    // li   $t1 1
    // addu $t2 $t0 $t1
    // ------------------
    // addiu $t2 $t0 1
    return std::make_unique<I_imm_Inst>(rOp_ImmOp(r.op), r.rd, r.rs, li.immediate);
}

namespace {

bool validReg(Register reg) {
    return reg != Register::none;
}

bool readsReg(const R_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::move:
        case Op::jr:
            return inst.rs == reg;
        case Op::clz:
            return inst.rs == reg;
        case Op::mfhi:
        case Op::none:
            return false;
        case Op::syscall:
            return reg == Register::v0 || reg == Register::a0 || reg == Register::a1;
        default:
            return inst.rs == reg || inst.rt == reg;
    }
}

bool writesReg(const R_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::addu:
        case Op::subu:
        case Op::mul:
        case Op::div:
        case Op::mfhi:
        case Op::and_:
        case Op::or_:
        case Op::add:
        case Op::slt:
        case Op::sle:
        case Op::sge:
        case Op::sgt:
        case Op::seq:
        case Op::sne:
        case Op::move:
        case Op::srav:
        case Op::clz:
            return inst.rd == reg;
        case Op::syscall:
            return reg == Register::v0;
        default:
            return false;
    }
}

bool readsReg(const I_imm_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::li:
            return false;
        case Op::sw:
        case Op::sb:
            return inst.rt == reg || inst.rs == reg;
        default:
            return inst.rs == reg;
    }
}

bool writesReg(const I_imm_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::sw:
        case Op::sb:
            return false;
        default:
            return inst.rt == reg;
    }
}

bool readsReg(const I_label_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::beq:
        case Op::bne:
        case Op::blt:
        case Op::ble:
        case Op::bge:
        case Op::bgt:
        case Op::bltu:
            return inst.rs == reg || inst.rt == reg;
        case Op::beqz:
        case Op::bgtz:
            return inst.rs == reg;
        case Op::sw:
        case Op::sb:
            return inst.rs == reg || inst.rt == reg;
        case Op::lw:
        case Op::lbu:
        case Op::la:
            return inst.rt == reg;
        default:
            return false;
    }
}

bool writesReg(const I_label_Inst &inst, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    switch (inst.op) {
        case Op::lw:
        case Op::lbu:
        case Op::la:
            return inst.rs == reg;
        default:
            return false;
    }
}

bool readsReg(const Assembly *assembly, Register reg) {
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_imm_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return readsReg(*inst, reg);
    }
    return false;
}

bool writesReg(const Assembly *assembly, Register reg) {
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_imm_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return writesReg(*inst, reg);
    }
    if (auto inst = dynamic_cast<const J_Inst *>(assembly)) {
        return inst->op == Op::jal && reg == Register::ra;
    }
    return false;
}

bool isControlBoundary(const Assembly *assembly) {
    if (dynamic_cast<const Label *>(assembly)) {
        return true;
    }
    if (dynamic_cast<const J_Inst *>(assembly)) {
        return true;
    }
    if (auto inst = dynamic_cast<const I_label_Inst *>(assembly)) {
        return inst->op == Op::beq || inst->op == Op::bne || inst->op == Op::blt
               || inst->op == Op::ble || inst->op == Op::bge || inst->op == Op::bgt
               || inst->op == Op::bltu || inst->op == Op::beqz || inst->op == Op::bgtz;
    }
    if (auto inst = dynamic_cast<const R_Inst *>(assembly)) {
        return inst->op == Op::jr;
    }
    return false;
}

bool simplifyConditionalBranches() {
    bool changed = false;
    for (size_t index = 0; index + 2 < assemblies.size();) {
        auto *branch = dynamic_cast<I_label_Inst *>(assemblies[index].get());
        auto *jump = dynamic_cast<J_Inst *>(assemblies[index + 1].get());
        auto *fallthrough = dynamic_cast<Label *>(assemblies[index + 2].get());
        if (!branch || !jump || !fallthrough || jump->op != Op::j
            || branch->label.nameAndId != fallthrough->nameAndId) {
            ++index;
            continue;
        }

        if (branch->op == Op::beq) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::bne, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::bne && branch->rt != Register::zero) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::beq, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::blt) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::bge, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::ble) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::bgt, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::bge) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::blt, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::bgt) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::ble, branch->rs, branch->rt, jump->label);
        } else if (branch->op == Op::bne && branch->rt == Register::zero) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::beqz, branch->rs, Register::none, jump->label);
        } else if (branch->op == Op::beqz) {
            assemblies[index] = std::make_unique<I_label_Inst>(
                    Op::bne, branch->rs, Register::zero, jump->label);
        } else {
            ++index;
            continue;
        }
        assemblies.erase(assemblies.begin() + static_cast<long>(index + 1));
        changed = true;
    }
    return changed;
}

bool eliminateRedundantInstructions() {
    bool changed = false;
    for (size_t index = 0; index < assemblies.size();) {
        if (auto *move = dynamic_cast<R_Inst *>(assemblies[index].get())) {
            if (move->op == Op::move && move->rd == move->rs) {
                assemblies.erase(assemblies.begin() + static_cast<long>(index));
                changed = true;
                continue;
            }
        }
        if (auto *immediate = dynamic_cast<I_imm_Inst *>(assemblies[index].get())) {
            if ((immediate->op == Op::addiu || immediate->op == Op::subiu)
                && immediate->immediate == 0 && immediate->rt == immediate->rs) {
                assemblies.erase(assemblies.begin() + static_cast<long>(index));
                changed = true;
                continue;
            }
        }
        if (auto *jump = dynamic_cast<J_Inst *>(assemblies[index].get())) {
            if (jump->op == Op::j) {
                bool reachesTarget = false;
                size_t next = index + 1;
                while (next < assemblies.size()) {
                    auto *label = dynamic_cast<Label *>(assemblies[next].get());
                    if (!label) {
                        break;
                    }
                    reachesTarget = reachesTarget || label->nameAndId == jump->label.nameAndId;
                    ++next;
                }
                if (reachesTarget) {
                    assemblies.erase(assemblies.begin() + static_cast<long>(index));
                    changed = true;
                    continue;
                }
            }
        }
        if (auto *branch = dynamic_cast<I_label_Inst *>(assemblies[index].get())) {
            if (branch->op == Op::beq || branch->op == Op::bne || branch->op == Op::blt
                || branch->op == Op::ble || branch->op == Op::bge || branch->op == Op::bgt
                || branch->op == Op::bltu || branch->op == Op::beqz || branch->op == Op::bgtz) {
                size_t next = index + 1;
                bool reachesTarget = false;
                while (next < assemblies.size()) {
                    auto *label = dynamic_cast<Label *>(assemblies[next].get());
                    if (!label) {
                        break;
                    }
                    reachesTarget = reachesTarget || label->nameAndId == branch->label.nameAndId;
                    ++next;
                }
                if (reachesTarget) {
                    assemblies.erase(assemblies.begin() + static_cast<long>(index));
                    changed = true;
                    continue;
                }
            }
        }
        ++index;
    }
    return changed;
}

bool batchRepeatedHalving() {
    static int batchLabelId = 0;

    // SSA phi lowering keeps the next accumulator in a separate register and
    // copies it on each backedge. Recognize that shape before the older
    // in-place accumulator form below.
    for (size_t index = 1; index + 5 < assemblies.size(); ++index) {
        auto *evenLabel = dynamic_cast<Label *>(assemblies[index].get());
        auto *sign = dynamic_cast<I_imm_Inst *>(assemblies[index + 1].get());
        auto *addBias = dynamic_cast<R_Inst *>(assemblies[index + 2].get());
        auto *divide = dynamic_cast<I_imm_Inst *>(assemblies[index + 3].get());
        auto *restoreAccumulator = dynamic_cast<R_Inst *>(assemblies[index + 4].get());
        auto *backedge = dynamic_cast<J_Inst *>(assemblies[index + 5].get());
        auto *oddBranch = dynamic_cast<I_label_Inst *>(assemblies[index - 1].get());
        if (!evenLabel || !sign || !addBias || !divide || !restoreAccumulator
            || !backedge || !oddBranch || sign->op != Op::srl || sign->immediate != 31
            || addBias->op != Op::addu || addBias->rd != sign->rs
            || addBias->rs != sign->rs || addBias->rt != sign->rt
            || divide->op != Op::sra || divide->rt != sign->rs
            || divide->rs != sign->rs || divide->immediate != 1
            || restoreAccumulator->op != Op::move || backedge->op != Op::j) {
            continue;
        }

        const Register stateReg = sign->rs;
        const Register accumulatorReg = restoreAccumulator->rd;
        const Register nextAccumulatorReg = restoreAccumulator->rs;
        size_t headerIndex = assemblies.size();
        for (size_t reverse = index; reverse-- > 0;) {
            auto *label = dynamic_cast<Label *>(assemblies[reverse].get());
            if (label && label->nameAndId == backedge->label.nameAndId) {
                headerIndex = reverse;
                break;
            }
        }
        if (headerIndex == assemblies.size() || headerIndex + 5 >= index) {
            continue;
        }

        auto *baseBranch = dynamic_cast<I_label_Inst *>(assemblies[headerIndex + 1].get());
        auto *baseTrue = dynamic_cast<Label *>(assemblies[headerIndex + 2].get());
        auto *returnMove = dynamic_cast<R_Inst *>(assemblies[headerIndex + 3].get());
        auto *returnJump = dynamic_cast<R_Inst *>(assemblies[headerIndex + 4].get());
        auto *baseFalse = dynamic_cast<Label *>(assemblies[headerIndex + 5].get());
        if (!baseBranch || !baseTrue || !returnMove || !returnJump || !baseFalse
            || baseBranch->op != Op::bne || baseBranch->rs != stateReg
            || baseBranch->label.nameAndId != baseFalse->nameAndId
            || returnMove->op != Op::move || returnMove->rd != Register::v0
            || returnMove->rs != accumulatorReg || returnJump->op != Op::jr
            || returnJump->rs != Register::ra) {
            continue;
        }

        I_imm_Inst *parity = nullptr;
        I_imm_Inst *increment = nullptr;
        for (size_t scan = headerIndex + 6; scan < index; ++scan) {
            auto *candidate = dynamic_cast<I_imm_Inst *>(assemblies[scan].get());
            if (!candidate) {
                continue;
            }
            if (candidate->op == Op::andi && candidate->rs == stateReg
                && candidate->immediate == 1) {
                parity = candidate;
            }
            if (candidate->op == Op::addiu && candidate->rt == nextAccumulatorReg
                && candidate->rs == accumulatorReg && candidate->immediate == 1) {
                increment = candidate;
            }
        }
        if (!parity || !increment || !readsReg(*oddBranch, parity->rt)) {
            continue;
        }

        bool comparesWithOne = false;
        for (size_t reverse = headerIndex; reverse-- > 0;) {
            if (writesReg(assemblies[reverse].get(), baseBranch->rt)) {
                auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[reverse].get());
                comparesWithOne = constant && constant->op == Op::li
                                  && constant->immediate == 1;
                break;
            }
        }
        if (!comparesWithOne) {
            continue;
        }

        const Label slow("__mips_div2_nonpositive_" + std::to_string(batchLabelId++));
        std::vector<std::unique_ptr<Assembly>> fastPath;
        fastPath.push_back(std::make_unique<I_label_Inst>(
                Op::ble, stateReg, Register::zero, slow));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::subu, parity->rt, Register::zero, stateReg));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::and_, parity->rt, parity->rt, stateReg));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::clz, parity->rt, parity->rt, Register::none));
        fastPath.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, parity->rt, parity->rt, -31));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::subu, parity->rt, Register::zero, parity->rt));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::srav, stateReg, stateReg, parity->rt));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::addu, nextAccumulatorReg, accumulatorReg, parity->rt));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::move, accumulatorReg, nextAccumulatorReg, Register::none));
        fastPath.push_back(std::make_unique<J_Inst>(Op::j, backedge->label));
        fastPath.push_back(std::make_unique<Label>(slow));
        assemblies.insert(assemblies.begin() + static_cast<long>(index + 1),
                          std::make_move_iterator(fastPath.begin()),
                          std::make_move_iterator(fastPath.end()));
        return true;
    }

    for (size_t index = 0; index + 5 < assemblies.size(); ++index) {
        auto *evenLabel = dynamic_cast<Label *>(assemblies[index].get());
        auto *increment = dynamic_cast<I_imm_Inst *>(assemblies[index + 1].get());
        auto *sign = dynamic_cast<I_imm_Inst *>(assemblies[index + 2].get());
        auto *addBias = dynamic_cast<R_Inst *>(assemblies[index + 3].get());
        auto *divide = dynamic_cast<I_imm_Inst *>(assemblies[index + 4].get());
        auto *backedge = dynamic_cast<J_Inst *>(assemblies[index + 5].get());
        if (!evenLabel || !increment || !sign || !addBias || !divide || !backedge
            || increment->op != Op::addiu || increment->rt != increment->rs
            || increment->immediate != 1 || sign->op != Op::srl || sign->immediate != 31
            || addBias->op != Op::addu || addBias->rd != sign->rs
            || addBias->rs != sign->rs || addBias->rt != sign->rt
            || divide->op != Op::sra || divide->rt != sign->rs
            || divide->rs != sign->rs || divide->immediate != 1
            || backedge->op != Op::j || index < 8) {
            continue;
        }

        auto *parity = dynamic_cast<I_imm_Inst *>(assemblies[index - 2].get());
        auto *oddBranch = dynamic_cast<I_label_Inst *>(assemblies[index - 1].get());
        if (!parity || !oddBranch || parity->op != Op::andi || parity->rs != sign->rs
            || parity->immediate != 1 || oddBranch->op != Op::bne
            || oddBranch->rs != parity->rt || oddBranch->rt != Register::zero) {
            continue;
        }

        size_t headerIndex = assemblies.size();
        for (size_t reverse = index; reverse-- > 0;) {
            auto *label = dynamic_cast<Label *>(assemblies[reverse].get());
            if (label && label->nameAndId == backedge->label.nameAndId) {
                headerIndex = reverse;
                break;
            }
        }
        if (headerIndex == assemblies.size() || headerIndex + 8 != index) {
            continue;
        }

        auto *baseBranch = dynamic_cast<I_label_Inst *>(assemblies[headerIndex + 1].get());
        auto *baseTrue = dynamic_cast<Label *>(assemblies[headerIndex + 2].get());
        auto *returnMove = dynamic_cast<R_Inst *>(assemblies[headerIndex + 3].get());
        auto *returnJump = dynamic_cast<R_Inst *>(assemblies[headerIndex + 4].get());
        auto *baseFalse = dynamic_cast<Label *>(assemblies[headerIndex + 5].get());
        if (!baseBranch || !baseTrue || !returnMove || !returnJump || !baseFalse
            || baseBranch->op != Op::bne || baseBranch->rs != sign->rs
            || baseBranch->label.nameAndId != baseFalse->nameAndId
            || returnMove->op != Op::move || returnMove->rd != Register::v0
            || returnMove->rs != increment->rt || returnJump->op != Op::jr
            || returnJump->rs != Register::ra) {
            continue;
        }

        bool comparesWithOne = false;
        for (size_t reverse = headerIndex; reverse-- > 0;) {
            if (writesReg(assemblies[reverse].get(), baseBranch->rt)) {
                auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[reverse].get());
                comparesWithOne = constant && constant->op == Op::li && constant->immediate == 1;
                break;
            }
            if (isControlBoundary(assemblies[reverse].get())) {
                break;
            }
        }
        if (!comparesWithOne) {
            continue;
        }

        const Label slow("__mips_div2_zero_" + std::to_string(batchLabelId++));
        std::vector<std::unique_ptr<Assembly>> fastPath;
        fastPath.push_back(std::make_unique<I_label_Inst>(
                Op::beqz, sign->rs, Register::none, slow));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::subu, parity->rt, Register::zero, sign->rs));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::and_, parity->rt, parity->rt, sign->rs));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::clz, parity->rt, parity->rt, Register::none));
        fastPath.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, parity->rt, parity->rt, -31));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::subu, parity->rt, Register::zero, parity->rt));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::srav, sign->rs, sign->rs, parity->rt));
        fastPath.push_back(std::make_unique<R_Inst>(
                Op::addu, increment->rt, increment->rt, parity->rt));
        fastPath.push_back(std::make_unique<I_label_Inst>(
                Op::beq, sign->rs, baseBranch->rt, *baseTrue));
        fastPath.push_back(std::make_unique<J_Inst>(Op::j, oddBranch->label));
        fastPath.push_back(std::make_unique<Label>(slow));
        assemblies.insert(assemblies.begin() + static_cast<long>(index + 1),
                          std::make_move_iterator(fastPath.begin()),
                          std::make_move_iterator(fastPath.end()));
        return true;
    }
    return false;
}

bool batchOddSuccessorHalving() {
    for (size_t index = 4; index + 3 < assemblies.size(); ++index) {
        auto *trueLabel = dynamic_cast<Label *>(assemblies[index].get());
        auto *increment = dynamic_cast<I_imm_Inst *>(assemblies[index + 1].get());
        auto *move = dynamic_cast<R_Inst *>(assemblies[index + 2].get());
        auto *backedge = dynamic_cast<J_Inst *>(assemblies[index + 3].get());
        auto *oddLabel = dynamic_cast<Label *>(assemblies[index - 4].get());
        auto *multiply = dynamic_cast<I_imm_Inst *>(assemblies[index - 3].get());
        auto *plusOne = dynamic_cast<I_imm_Inst *>(assemblies[index - 2].get());
        auto *rangeBranch = dynamic_cast<I_label_Inst *>(assemblies[index - 1].get());
        if (!trueLabel || !increment || !move || !backedge || !oddLabel || !multiply
            || !plusOne || !rangeBranch || increment->op != Op::addiu
            || increment->rt != increment->rs || increment->immediate != 1
            || move->op != Op::move || backedge->op != Op::j
            || multiply->op != Op::mul || multiply->immediate != 3
            || plusOne->op != Op::addiu || plusOne->rs != multiply->rt
            || plusOne->immediate != 1 || rangeBranch->op != Op::bgt
            || rangeBranch->rs != plusOne->rt
            || move->rd != multiply->rs || move->rs != plusOne->rt) {
            continue;
        }

        size_t headerIndex = assemblies.size();
        for (size_t reverse = index; reverse-- > 0;) {
            auto *label = dynamic_cast<Label *>(assemblies[reverse].get());
            if (label && label->nameAndId == backedge->label.nameAndId) {
                headerIndex = reverse;
                break;
            }
        }
        if (headerIndex == assemblies.size() || headerIndex + 7 >= assemblies.size()) {
            continue;
        }

        auto *baseBranch = dynamic_cast<I_label_Inst *>(assemblies[headerIndex + 1].get());
        auto *returnMove = dynamic_cast<R_Inst *>(assemblies[headerIndex + 3].get());
        auto *returnJump = dynamic_cast<R_Inst *>(assemblies[headerIndex + 4].get());
        auto *baseFalse = dynamic_cast<Label *>(assemblies[headerIndex + 5].get());
        auto *parity = dynamic_cast<I_imm_Inst *>(assemblies[headerIndex + 6].get());
        auto *oddBranch = dynamic_cast<I_label_Inst *>(assemblies[headerIndex + 7].get());
        if (!baseBranch || !returnMove || !returnJump || !baseFalse || !parity || !oddBranch
            || baseBranch->op != Op::bne || baseBranch->rs != multiply->rs
            || baseBranch->label.nameAndId != baseFalse->nameAndId
            || returnMove->op != Op::move || returnMove->rd != Register::v0
            || returnMove->rs != increment->rt || returnJump->op != Op::jr
            || returnJump->rs != Register::ra || parity->op != Op::andi
            || parity->rs != multiply->rs || parity->immediate != 1
            || oddBranch->op != Op::bne || oddBranch->rs != parity->rt
            || oddBranch->rt != Register::zero
            || oddBranch->label.nameAndId != oddLabel->nameAndId) {
            continue;
        }

        bool comparesWithOne = false;
        for (size_t reverse = headerIndex; reverse-- > 0;) {
            if (writesReg(assemblies[reverse].get(), baseBranch->rt)) {
                auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[reverse].get());
                comparesWithOne = constant && constant->op == Op::li && constant->immediate == 1;
                break;
            }
            if (isControlBoundary(assemblies[reverse].get())) {
                break;
            }
        }
        if (!comparesWithOne) {
            continue;
        }

        increment->immediate = 2;
        assemblies[index + 2] = std::make_unique<I_imm_Inst>(
                Op::sra, move->rd, move->rs, 1);
        return true;
    }
    return false;
}

bool memoizeBoundedTailRecurrences(const IR::Module &module) {
    static int memoLabelId = 0;
    // Larger tables cost more to initialize and are slower in Mars' sparse
    // memory simulation than the additional cache hits save.
    constexpr int maxMemoizedState = (3 << 17) - 1;

    auto mentions = [](const Assembly *assembly, Register reg) {
        return readsReg(assembly, reg) || writesReg(assembly, reg);
    };
    auto isCalleeSaved = [](Register reg) {
        return reg >= Register::s0 && reg <= Register::s7;
    };
    auto isPreservedStackAccess = [&](const Assembly *assembly) {
        const auto *memory = dynamic_cast<const I_imm_Inst *>(assembly);
        return memory && (memory->op == Op::lw || memory->op == Op::sw)
               && memory->rs == Register::sp
               && (isCalleeSaved(memory->rt) || memory->rt == Register::ra);
    };
    for (const auto &assembly: assemblies) {
        if (mentions(assembly.get(), Register::s2) || mentions(assembly.get(), Register::s3)
            || mentions(assembly.get(), Register::s4) || mentions(assembly.get(), Register::s5)
            || mentions(assembly.get(), Register::s6)) {
            return false;
        }
    }

    bool changed = false;
    for (const auto &function: module.getFunctions()) {
        if (function->getParams().size() != 2 || !usesRegisterArguments(*function)) {
            continue;
        }

        size_t functionStart = assemblies.size();
        size_t functionEnd = assemblies.size();
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *label = dynamic_cast<Label *>(assemblies[index].get());
            if (!label) {
                continue;
            }
            if (label->nameAndId == function->getName()) {
                functionStart = index;
                continue;
            }
            if (functionStart != assemblies.size()) {
                for (const auto &other: module.getFunctions()) {
                    if (label->nameAndId == other->getName()) {
                        functionEnd = index;
                        break;
                    }
                }
                if (functionEnd != assemblies.size()) {
                    break;
                }
            }
        }
        if (functionStart == assemblies.size()) {
            continue;
        }

        size_t callIndex = assemblies.size();
        int callCount = 0;
        for (size_t index = 0; index < functionStart; ++index) {
            auto *call = dynamic_cast<J_Inst *>(assemblies[index].get());
            if (call && call->op == Op::jal && call->label.nameAndId == function->getName()) {
                callIndex = index;
                ++callCount;
            }
        }
        if (callCount != 1) {
            continue;
        }

        bool callIsInLoop = false;
        size_t hotLoopBackedge = assemblies.size();
        std::string hotLoopHeader;
        for (size_t index = callIndex + 1; index < functionStart; ++index) {
            auto *backedge = dynamic_cast<J_Inst *>(assemblies[index].get());
            if (!backedge || backedge->op != Op::j) {
                continue;
            }
            for (size_t target = 0; target < callIndex; ++target) {
                auto *label = dynamic_cast<Label *>(assemblies[target].get());
                if (label && label->nameAndId == backedge->label.nameAndId) {
                    callIsInLoop = true;
                    hotLoopBackedge = index;
                    hotLoopHeader = label->nameAndId;
                    break;
                }
            }
            if (callIsInLoop) {
                break;
            }
        }
        if (!callIsInLoop) {
            continue;
        }

        Register loopConstantReg = Register::none;
        int loopConstantValue = 0;
        for (size_t index = callIndex + 1; index + 1 < hotLoopBackedge; ++index) {
            auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
            auto *use = dynamic_cast<I_label_Inst *>(assemblies[index + 1].get());
            if (!constant || !use || constant->op != Op::li
                || std::abs(static_cast<long long>(constant->immediate)) <= INT16_MAX
                || (use->rs != constant->rt && use->rt != constant->rt)) {
                continue;
            }
            bool overwritten = false;
            for (size_t later = index + 1; later < hotLoopBackedge; ++later) {
                if (writesReg(assemblies[later].get(), constant->rt)) {
                    overwritten = true;
                    break;
                }
            }
            if (!overwritten) {
                loopConstantReg = constant->rt;
                loopConstantValue = constant->immediate;
                break;
            }
        }

        size_t headerIndex = assemblies.size();
        Register stateReg = Register::none;
        Register accumulatorReg = Register::none;
        Register oneReg = Register::none;
        std::string headerName;
        std::string baseTrueName;
        size_t baseReturnStart = assemblies.size();
        for (size_t index = functionStart + 1; index + 1 < functionEnd; ++index) {
            auto *header = dynamic_cast<Label *>(assemblies[index].get());
            auto *baseBranch = dynamic_cast<I_label_Inst *>(assemblies[index + 1].get());
            if (!header || !baseBranch
                || (baseBranch->op != Op::beq && baseBranch->op != Op::bne)) {
                continue;
            }

            std::string candidateBaseLabel;
            if (baseBranch->op == Op::beq) {
                candidateBaseLabel = baseBranch->label.nameAndId;
            } else {
                for (size_t next = index + 2; next < functionEnd; ++next) {
                    if (auto *fallthrough = dynamic_cast<Label *>(assemblies[next].get())) {
                        candidateBaseLabel = fallthrough->nameAndId;
                        break;
                    }
                }
            }
            size_t candidateBaseBlock = assemblies.size();
            for (size_t candidate = functionStart + 1; candidate < functionEnd; ++candidate) {
                auto *label = dynamic_cast<Label *>(assemblies[candidate].get());
                if (label && label->nameAndId == candidateBaseLabel) {
                    candidateBaseBlock = candidate;
                    break;
                }
            }
            if (candidateBaseBlock == assemblies.size()) {
                continue;
            }

            Register candidateAccumulator = Register::none;
            size_t candidateReturnMove = assemblies.size();
            bool candidateReturns = false;
            for (size_t body = candidateBaseBlock + 1; body < functionEnd; ++body) {
                if (dynamic_cast<Label *>(assemblies[body].get())) {
                    break;
                }
                auto *move = dynamic_cast<R_Inst *>(assemblies[body].get());
                if (move && move->op == Op::move && move->rd == Register::v0) {
                    candidateAccumulator = move->rs;
                    candidateReturnMove = body;
                }
                if (move && move->op == Op::jr && move->rs == Register::ra) {
                    candidateReturns = true;
                    break;
                }
            }
            if (!candidateReturns || candidateAccumulator == Register::none) {
                continue;
            }

            bool hasParityDispatch = false;
            for (size_t parityIndex = index + 2; parityIndex < functionEnd; ++parityIndex) {
                auto *parity = dynamic_cast<I_imm_Inst *>(assemblies[parityIndex].get());
                if (!parity || parity->op != Op::andi || parity->rs != baseBranch->rs
                    || parity->immediate != 1) {
                    continue;
                }
                bool parityValueAvailable = true;
                for (size_t branchIndex = parityIndex + 1; branchIndex < functionEnd;
                     ++branchIndex) {
                    Assembly *candidate = assemblies[branchIndex].get();
                    if (dynamic_cast<Label *>(candidate)) {
                        break;
                    }
                    if (auto *branch = dynamic_cast<I_label_Inst *>(candidate)) {
                        const bool conditional = branch->op == Op::beq || branch->op == Op::bne
                                                 || branch->op == Op::blt || branch->op == Op::ble
                                                 || branch->op == Op::bge || branch->op == Op::bgt
                                                 || branch->op == Op::bltu || branch->op == Op::beqz
                                                 || branch->op == Op::bgtz;
                        if (conditional && readsReg(candidate, parity->rt)) {
                            hasParityDispatch = parityValueAvailable;
                            break;
                        }
                    }
                    if (writesReg(candidate, parity->rt)
                        && !readsReg(candidate, parity->rt)) {
                        parityValueAvailable = false;
                        break;
                    }
                }
                break;
            }
            if (!hasParityDispatch) {
                continue;
            }
            headerIndex = index;
            stateReg = baseBranch->rs;
            oneReg = baseBranch->rt;
            accumulatorReg = candidateAccumulator;
            headerName = header->nameAndId;
            baseTrueName = candidateBaseLabel;
            baseReturnStart = candidateReturnMove;
            break;
        }
        if (headerIndex == assemblies.size() || stateReg == accumulatorReg
            || stateReg == Register::none || accumulatorReg == Register::none
            || baseReturnStart == assemblies.size()) {
            continue;
        }

        bool oneIsConstant = false;
        size_t initialOneDefinition = assemblies.size();
        for (size_t reverse = headerIndex; reverse-- > functionStart;) {
            if (writesReg(assemblies[reverse].get(), oneReg)) {
                auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[reverse].get());
                oneIsConstant = constant && constant->op == Op::li && constant->immediate == 1;
                initialOneDefinition = reverse;
                break;
            }
        }
        if (!oneIsConstant) {
            continue;
        }

        Register memoStartState = Register::a2;
        Register memoStartAccumulator = Register::a3;
        Register stateArgumentSource = Register::none;
        for (size_t reverse = callIndex; reverse-- > 0;) {
            Assembly *candidate = assemblies[reverse].get();
            if (writesReg(candidate, stateReg)) {
                auto *stateArgument = dynamic_cast<R_Inst *>(candidate);
                if (stateArgument && stateArgument->op == Op::move
                    && stateArgument->rd == stateReg) {
                    stateArgumentSource = stateArgument->rs;
                }
                bool clobberedByCallee = false;
                for (size_t index = functionStart; index < functionEnd; ++index) {
                    if (writesReg(assemblies[index].get(), stateArgumentSource)) {
                        if (index == initialOneDefinition
                            || isPreservedStackAccess(assemblies[index].get())) {
                            continue;
                        }
                        clobberedByCallee = true;
                        break;
                    }
                }
                if (stateArgumentSource != Register::none && !clobberedByCallee) {
                    memoStartState = stateArgumentSource;
                }
                break;
            }
            if (isControlBoundary(candidate)) {
                break;
            }
        }
        for (size_t reverse = callIndex; reverse-- > 0;) {
            Assembly *candidate = assemblies[reverse].get();
            if (writesReg(candidate, accumulatorReg)) {
                auto *constant = dynamic_cast<I_imm_Inst *>(candidate);
                if (constant && constant->op == Op::li && constant->rt == accumulatorReg
                    && constant->immediate == 0) {
                    memoStartAccumulator = Register::zero;
                }
                break;
            }
            if (isControlBoundary(candidate)) {
                break;
            }
        }
        if (memoStartState == oneReg && stateArgumentSource == oneReg) {
            const bool s7Available = std::none_of(
                    assemblies.begin(), assemblies.end(), [&](const auto &assembly) {
                        return mentions(assembly.get(), Register::s7);
                    });
            size_t callerStart = assemblies.size();
            size_t callerEnd = assemblies.size();
            for (size_t reverse = callIndex + 1; reverse-- > 0;) {
                auto *label = dynamic_cast<Label *>(assemblies[reverse].get());
                if (!label) {
                    continue;
                }
                bool entry = label->nameAndId == "main";
                for (const auto &other: module.getFunctions()) {
                    entry = entry || label->nameAndId == other->getName();
                }
                if (entry) {
                    callerStart = reverse;
                    break;
                }
            }
            for (size_t index = callIndex + 1; index < assemblies.size(); ++index) {
                auto *label = dynamic_cast<Label *>(assemblies[index].get());
                if (!label) {
                    continue;
                }
                bool entry = label->nameAndId == "main";
                for (const auto &other: module.getFunctions()) {
                    entry = entry || label->nameAndId == other->getName();
                }
                if (entry) {
                    callerEnd = index;
                    break;
                }
            }
            if (s7Available && callerStart != assemblies.size()
                && callerEnd != assemblies.size()) {
                for (size_t index = callerStart + 1; index < callerEnd; ++index) {
                    if (auto *inst = dynamic_cast<R_Inst *>(assemblies[index].get())) {
                        if (inst->rd == oneReg) {
                            inst->rd = Register::s7;
                        }
                        if (inst->rs == oneReg) {
                            inst->rs = Register::s7;
                        }
                        if (inst->rt == oneReg) {
                            inst->rt = Register::s7;
                        }
                    } else if (auto *inst = dynamic_cast<I_imm_Inst *>(assemblies[index].get())) {
                        if (inst->rt == oneReg) {
                            inst->rt = Register::s7;
                        }
                        if (inst->rs == oneReg) {
                            inst->rs = Register::s7;
                        }
                    } else if (auto *inst = dynamic_cast<I_label_Inst *>(assemblies[index].get())) {
                        if (inst->rs == oneReg) {
                            inst->rs = Register::s7;
                        }
                        if (inst->rt == oneReg) {
                            inst->rt = Register::s7;
                        }
                    }
                }
                stateArgumentSource = Register::s7;
                memoStartState = Register::s7;
            } else {
                memoStartState = Register::a2;
            }
        }

        Register limitReg = Register::none;
        std::string limitName;
        int globalLoads = 0;
        bool hasMemoryOrSideEffects = false;
        int returns = 0;
        size_t constantReturnStart = assemblies.size();
        int constantReturnValue = 0;

        size_t accumulatorUpdate = assemblies.size();
        Register nextAccumulatorReg = accumulatorReg;
        for (size_t index = headerIndex + 1; index < functionEnd; ++index) {
            auto *update = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
            if (update && update->op == Op::addiu && update->rs == accumulatorReg
                && update->immediate >= 0) {
                accumulatorUpdate = index;
                nextAccumulatorReg = update->rt;
                break;
            }
        }
        if (nextAccumulatorReg != accumulatorReg && isCalleeSaved(nextAccumulatorReg)
            && memoStartAccumulator == Register::zero) {
            const bool argumentScratchAvailable = std::none_of(
                    assemblies.begin() + static_cast<long>(functionStart + 1),
                    assemblies.begin() + static_cast<long>(functionEnd),
                    [&](const auto &assembly) {
                        return mentions(assembly.get(), Register::a3);
                    });
            if (argumentScratchAvailable) {
                const Register from = nextAccumulatorReg;
                for (size_t index = functionStart + 1; index < functionEnd; ++index) {
                    Assembly *assembly = assemblies[index].get();
                    if (isPreservedStackAccess(assembly)) {
                        continue;
                    }
                    if (auto *inst = dynamic_cast<R_Inst *>(assembly)) {
                        if (inst->rd == from) {
                            inst->rd = Register::a3;
                        }
                        if (inst->rs == from) {
                            inst->rs = Register::a3;
                        }
                        if (inst->rt == from) {
                            inst->rt = Register::a3;
                        }
                    } else if (auto *inst = dynamic_cast<I_imm_Inst *>(assembly)) {
                        if (inst->rt == from) {
                            inst->rt = Register::a3;
                        }
                        if (inst->rs == from) {
                            inst->rs = Register::a3;
                        }
                    } else if (auto *inst = dynamic_cast<I_label_Inst *>(assembly)) {
                        if (inst->rs == from) {
                            inst->rs = Register::a3;
                        }
                        if (inst->rt == from) {
                            inst->rt = Register::a3;
                        }
                    }
                }
                nextAccumulatorReg = Register::a3;
            }
        }
        auto isBatchedDerivedUpdate = [&](size_t index) {
            auto *update = index < assemblies.size()
                                   ? dynamic_cast<R_Inst *>(assemblies[index].get())
                                   : nullptr;
            if (!update || update->op != Op::addu || update->rd != nextAccumulatorReg
                || update->rs != accumulatorReg || index < 6) {
                return false;
            }
            const Register delta = update->rt;
            auto *negState = dynamic_cast<R_Inst *>(assemblies[index - 6].get());
            auto *lowBit = dynamic_cast<R_Inst *>(assemblies[index - 5].get());
            auto *countZeros = dynamic_cast<R_Inst *>(assemblies[index - 4].get());
            auto *bias = dynamic_cast<I_imm_Inst *>(assemblies[index - 3].get());
            auto *negCount = dynamic_cast<R_Inst *>(assemblies[index - 2].get());
            auto *shiftState = dynamic_cast<R_Inst *>(assemblies[index - 1].get());
            return negState && lowBit && countZeros && bias && negCount && shiftState
                   && negState->op == Op::subu && negState->rd == delta
                   && negState->rs == Register::zero && negState->rt == stateReg
                   && lowBit->op == Op::and_ && lowBit->rd == delta
                   && lowBit->rs == delta && lowBit->rt == stateReg
                   && countZeros->op == Op::clz && countZeros->rd == delta
                   && countZeros->rs == delta && bias->op == Op::addiu
                   && bias->rt == delta && bias->rs == delta && bias->immediate == -31
                   && negCount->op == Op::subu && negCount->rd == delta
                   && negCount->rs == Register::zero && negCount->rt == delta
                   && shiftState->op == Op::srav && shiftState->rd == stateReg
                   && shiftState->rs == stateReg && shiftState->rt == delta;
        };
        if (nextAccumulatorReg != accumulatorReg) {
            int backedges = 0;
            for (size_t index = accumulatorUpdate + 1; index < functionEnd; ++index) {
                Assembly *assembly = assemblies[index].get();
                auto *restore = dynamic_cast<R_Inst *>(assembly);
                const bool restoresAccumulator = restore && restore->op == Op::move
                                                 && restore->rd == accumulatorReg
                                                 && restore->rs == nextAccumulatorReg;
                const bool batchedUpdate = isBatchedDerivedUpdate(index);
                if ((readsReg(assembly, nextAccumulatorReg) && !restoresAccumulator)
                    || (writesReg(assembly, nextAccumulatorReg) && !batchedUpdate
                        && !isPreservedStackAccess(assembly))) {
                    hasMemoryOrSideEffects = true;
                    break;
                }

                auto *backedge = dynamic_cast<J_Inst *>(assembly);
                if (!backedge || backedge->op != Op::j
                    || backedge->label.nameAndId != headerName) {
                    continue;
                }
                ++backedges;
                bool restoredOnEdge = false;
                for (size_t reverse = index; reverse-- > accumulatorUpdate + 1;) {
                    if (dynamic_cast<Label *>(assemblies[reverse].get())) {
                        break;
                    }
                    auto *move = dynamic_cast<R_Inst *>(assemblies[reverse].get());
                    if (move && move->op == Op::move && move->rd == accumulatorReg
                        && move->rs == nextAccumulatorReg) {
                        restoredOnEdge = true;
                        break;
                    }
                }
                if (!restoredOnEdge) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
            }
            if (backedges == 0) {
                hasMemoryOrSideEffects = true;
            }
        }

        for (size_t index = functionStart + 1; index < functionEnd; ++index) {
            if (mentions(assemblies[index].get(), Register::a2)
                || (nextAccumulatorReg != Register::a3
                    && mentions(assemblies[index].get(), Register::a3))) {
                hasMemoryOrSideEffects = true;
                break;
            }
            if (auto *inst = dynamic_cast<I_imm_Inst *>(assemblies[index].get())) {
                if (inst->op == Op::lw || inst->op == Op::lbu || inst->op == Op::sw
                    || inst->op == Op::sb) {
                    if (!isPreservedStackAccess(inst)) {
                        hasMemoryOrSideEffects = true;
                        break;
                    }
                    continue;
                }
                const bool affineUpdate = inst->op == Op::addiu
                                          && inst->rs == accumulatorReg
                                          && (inst->rt == accumulatorReg
                                              || (index == accumulatorUpdate
                                                  && inst->rt == nextAccumulatorReg))
                                          && inst->immediate >= 0;
                const bool detachedAccumulator = nextAccumulatorReg != accumulatorReg
                                                 && index > accumulatorUpdate;
                if (index >= headerIndex && !detachedAccumulator
                    && (readsReg(*inst, accumulatorReg) || writesReg(*inst, accumulatorReg))
                    && !affineUpdate) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
            }
            if (auto *inst = dynamic_cast<I_label_Inst *>(assemblies[index].get())) {
                if (inst->op == Op::lw && inst->rt == Register::none) {
                    ++globalLoads;
                    limitReg = inst->rs;
                    limitName = inst->label.nameAndId;
                } else if (inst->op == Op::lw || inst->op == Op::lbu || inst->op == Op::sw
                           || inst->op == Op::sb || inst->op == Op::la) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
                const bool detachedAccumulator = nextAccumulatorReg != accumulatorReg
                                                 && index > accumulatorUpdate;
                if (index >= headerIndex && !detachedAccumulator
                    && (inst->rs == accumulatorReg || inst->rt == accumulatorReg)) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
            }
            if (auto *inst = dynamic_cast<R_Inst *>(assemblies[index].get())) {
                if (inst->op == Op::syscall) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
                const bool returnsAccumulator = inst->op == Op::move
                                                && inst->rd == Register::v0
                                                && inst->rs == accumulatorReg;
                const bool restoresAccumulator = nextAccumulatorReg != accumulatorReg
                                                 && inst->op == Op::move
                                                 && inst->rd == accumulatorReg
                                                 && inst->rs == nextAccumulatorReg;
                const bool detachedAccumulator = nextAccumulatorReg != accumulatorReg
                                                 && index > accumulatorUpdate;
                bool affineUpdate = inst->op == Op::addu && inst->rd == accumulatorReg
                                    && ((inst->rs == accumulatorReg)
                                        != (inst->rt == accumulatorReg));
                if (affineUpdate) {
                    Register delta = inst->rs == accumulatorReg ? inst->rt : inst->rs;
                    auto *negState = index >= 6
                                             ? dynamic_cast<R_Inst *>(assemblies[index - 6].get())
                                             : nullptr;
                    auto *lowBit = index >= 5
                                           ? dynamic_cast<R_Inst *>(assemblies[index - 5].get())
                                           : nullptr;
                    auto *countZeros = index >= 4
                                               ? dynamic_cast<R_Inst *>(assemblies[index - 4].get())
                                               : nullptr;
                    auto *bias = index >= 3
                                         ? dynamic_cast<I_imm_Inst *>(assemblies[index - 3].get())
                                         : nullptr;
                    auto *negCount = index >= 2
                                             ? dynamic_cast<R_Inst *>(assemblies[index - 2].get())
                                             : nullptr;
                    auto *shiftState = index >= 1
                                               ? dynamic_cast<R_Inst *>(assemblies[index - 1].get())
                                               : nullptr;
                    affineUpdate = negState && lowBit && countZeros && bias && negCount
                                   && shiftState
                                   && negState->op == Op::subu && negState->rd == delta
                                   && negState->rs == Register::zero && negState->rt == stateReg
                                   && lowBit->op == Op::and_ && lowBit->rd == delta
                                   && lowBit->rs == delta && lowBit->rt == stateReg
                                   && countZeros->op == Op::clz && countZeros->rd == delta
                                   && countZeros->rs == delta && bias->op == Op::addiu
                                   && bias->rt == delta && bias->rs == delta
                                   && bias->immediate == -31 && negCount->op == Op::subu
                                   && negCount->rd == delta && negCount->rs == Register::zero
                                   && negCount->rt == delta && shiftState->op == Op::srav
                                   && shiftState->rd == stateReg && shiftState->rs == stateReg
                                   && shiftState->rt == delta;
                }
                if (index >= headerIndex && !detachedAccumulator
                    && (readsReg(*inst, accumulatorReg) || writesReg(*inst, accumulatorReg))
                    && !returnsAccumulator && !restoresAccumulator && !affineUpdate) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
                if (inst->op == Op::jr && inst->rs == Register::ra) {
                    ++returns;
                    size_t blockStart = index;
                    while (blockStart > functionStart + 1
                           && !dynamic_cast<Label *>(assemblies[blockStart - 1].get())) {
                        --blockStart;
                    }
                    size_t valueMove = assemblies.size();
                    Register valueReg = Register::none;
                    for (size_t reverse = index; reverse-- > blockStart;) {
                        auto *move = dynamic_cast<R_Inst *>(assemblies[reverse].get());
                        if (move && move->op == Op::move && move->rd == Register::v0) {
                            valueMove = reverse;
                            valueReg = move->rs;
                            break;
                        }
                    }
                    if (valueMove != assemblies.size()) {
                        for (size_t reverse = valueMove; reverse-- > blockStart;) {
                            if (!writesReg(assemblies[reverse].get(), valueReg)) {
                                continue;
                            }
                            auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[reverse].get());
                            if (constant && constant->op == Op::li
                                && constant->rt == valueReg) {
                                constantReturnStart = reverse;
                                constantReturnValue = constant->immediate;
                            }
                            break;
                        }
                    }
                }
            }
            if (auto *inst = dynamic_cast<J_Inst *>(assemblies[index].get())) {
                if (inst->op == Op::jal) {
                    hasMemoryOrSideEffects = true;
                    break;
                }
            }
        }
        if (hasMemoryOrSideEffects || globalLoads != 1 || limitReg == Register::none
            || returns != 2 || constantReturnStart == assemblies.size()
            || constantReturnValue < 0 || constantReturnValue == INT_MAX) {
            continue;
        }

        bool hasBoundCheck = false;
        for (size_t index = headerIndex; index < functionEnd; ++index) {
            auto *branch = dynamic_cast<I_label_Inst *>(assemblies[index].get());
            if (branch && (branch->op == Op::bgt || branch->op == Op::ble)
                && branch->rt == limitReg) {
                hasBoundCheck = true;
                break;
            }
        }
        if (!hasBoundCheck) {
            continue;
        }

        size_t limitStore = assemblies.size();
        Register limitValueReg = Register::none;
        int limitStores = 0;
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *store = dynamic_cast<I_label_Inst *>(assemblies[index].get());
            if (store && store->op == Op::sw && store->rt == Register::none
                && store->label.nameAndId == limitName) {
                limitStore = index;
                limitValueReg = store->rs;
                ++limitStores;
            }
        }
        if (limitStores != 1 || limitStore >= callIndex || limitValueReg == Register::none) {
            continue;
        }

        std::vector<std::pair<Register, int>> preservedRegisters;
        for (size_t index = functionStart + 1; index < headerIndex; ++index) {
            auto *store = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
            if (store && store->op == Op::sw && store->rs == Register::sp
                && (isCalleeSaved(store->rt) || store->rt == Register::ra)) {
                preservedRegisters.emplace_back(store->rt, store->immediate);
            }
        }

        struct CallerSpill {
            size_t store;
            size_t load;
            Register value;
        };
        std::unordered_map<int, std::pair<size_t, Register>> callerStores;
        for (size_t reverse = callIndex; reverse-- > 0;) {
            Assembly *candidate = assemblies[reverse].get();
            auto *store = dynamic_cast<I_imm_Inst *>(candidate);
            if (store && store->op == Op::sw && store->rs == Register::sp) {
                callerStores.emplace(store->immediate, std::make_pair(reverse, store->rt));
                continue;
            }
            if (isControlBoundary(candidate)) {
                break;
            }
        }

        std::vector<CallerSpill> callerSpills;
        for (size_t index = callIndex + 1; index < functionStart; ++index) {
            Assembly *candidate = assemblies[index].get();
            auto *load = dynamic_cast<I_imm_Inst *>(candidate);
            if (load && load->op == Op::lw && load->rs == Register::sp) {
                auto store = callerStores.find(load->immediate);
                if (store != callerStores.end() && store->second.second == load->rt) {
                    callerSpills.push_back({store->second.first, index, load->rt});
                }
                continue;
            }
            if (isControlBoundary(candidate)) {
                break;
            }
        }

        std::vector<Register> preserveRegisters;
        for (Register candidate: {Register::s0, Register::s1, Register::s7}) {
            const bool used = std::any_of(assemblies.begin(), assemblies.end(), [&](const auto &assembly) {
                return mentions(assembly.get(), candidate);
            });
            if (!used) {
                preserveRegisters.push_back(candidate);
            }
        }

        bool callerIsMain = false;
        size_t callerStart = assemblies.size();
        for (size_t reverse = callIndex + 1; reverse-- > 0;) {
            auto *label = dynamic_cast<Label *>(assemblies[reverse].get());
            if (!label) {
                continue;
            }
            bool functionEntry = label->nameAndId == "main";
            for (const auto &other: module.getFunctions()) {
                functionEntry = functionEntry || label->nameAndId == other->getName();
            }
            if (functionEntry) {
                callerStart = reverse;
                callerIsMain = label->nameAndId == "main";
                break;
            }
        }

        auto replaceRegister = [](Assembly *assembly, Register from, Register to) {
            if (auto *inst = dynamic_cast<R_Inst *>(assembly)) {
                if (inst->rd == from) {
                    inst->rd = to;
                }
                if (inst->rs == from) {
                    inst->rs = to;
                }
                if (inst->rt == from) {
                    inst->rt = to;
                }
            } else if (auto *inst = dynamic_cast<I_imm_Inst *>(assembly)) {
                if (inst->rt == from) {
                    inst->rt = to;
                }
                if (inst->rs == from) {
                    inst->rs = to;
                }
            } else if (auto *inst = dynamic_cast<I_label_Inst *>(assembly)) {
                if (inst->rs == from) {
                    inst->rs = to;
                }
                if (inst->rt == from) {
                    inst->rt = to;
                }
            }
        };
        for (size_t index = 0;
             index < callerSpills.size() && index < preserveRegisters.size(); ++index) {
            const auto &spill = callerSpills[index];
            const Register preserved = preserveRegisters[index];
            if (callerIsMain && callerStart != assemblies.size()) {
                for (size_t assembly = callerStart + 1; assembly < functionStart; ++assembly) {
                    replaceRegister(assemblies[assembly].get(), spill.value, preserved);
                }
                assemblies[spill.store] = std::make_unique<R_Inst>(
                        Op::move, preserved, preserved, Register::none);
                assemblies[spill.load] = std::make_unique<R_Inst>(
                        Op::move, preserved, preserved, Register::none);
                if (limitValueReg == spill.value) {
                    limitValueReg = preserved;
                }
            } else {
                assemblies[spill.store] = std::make_unique<R_Inst>(
                        Op::move, preserved, spill.value, Register::none);
                assemblies[spill.load] = std::make_unique<R_Inst>(
                        Op::move, spill.value, preserved, Register::none);
            }
            if (spill.value == stateArgumentSource) {
                memoStartState = preserved;
                stateArgumentSource = preserved;
            }
        }

        const int id = memoLabelId++;
        const Label cacheDisabled("__mips_memo_disabled_" + std::to_string(id));
        const Label cacheAllocationFailed("__mips_memo_alloc_failed_" + std::to_string(id));
        const Label cacheBoundReady("__mips_memo_bound_ready_" + std::to_string(id));
        const Label initLoop("__mips_memo_init_" + std::to_string(id));
        const Label cacheMiss("__mips_memo_miss_" + std::to_string(id));
        const Label constantHit("__mips_memo_constant_hit_" + std::to_string(id));
        const Label affineHitReturn("__mips_memo_affine_return_" + std::to_string(id));
        const Label constantHitReturn("__mips_memo_constant_return_" + std::to_string(id));
        const Label baseNoStore("__mips_memo_base_no_store_" + std::to_string(id));
        const Label constantNoStore("__mips_memo_constant_no_store_" + std::to_string(id));

        std::vector<std::unique_ptr<Assembly>> constantStore;
        constantStore.push_back(std::make_unique<I_label_Inst>(
                Op::bltu, Register::s3, memoStartState, constantNoStore));
        constantStore.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t8, memoStartState, 2));
        constantStore.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::t9, Register::none, -(constantReturnValue + 1)));
        constantStore.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::fp, Register::s2, Register::t8));
        constantStore.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::t9, Register::fp, 0));
        constantStore.push_back(std::make_unique<Label>(constantNoStore));
        const size_t constantStoreSize = constantStore.size();
        assemblies.insert(assemblies.begin() + static_cast<long>(constantReturnStart),
                          std::make_move_iterator(constantStore.begin()),
                          std::make_move_iterator(constantStore.end()));
        if (constantReturnStart <= baseReturnStart) {
            baseReturnStart += constantStoreSize;
        }
        if (constantReturnStart <= headerIndex) {
            headerIndex += constantStoreSize;
        }

        std::vector<std::unique_ptr<Assembly>> baseStore;
        baseStore.push_back(std::make_unique<I_label_Inst>(
                Op::bltu, Register::s3, memoStartState, baseNoStore));
        baseStore.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t8, memoStartState, 2));
        if (memoStartAccumulator == Register::zero) {
            baseStore.push_back(std::make_unique<I_imm_Inst>(
                    Op::addiu, Register::t9, accumulatorReg, 1));
        } else {
            baseStore.push_back(std::make_unique<R_Inst>(
                    Op::subu, Register::t9, accumulatorReg, memoStartAccumulator));
            baseStore.push_back(std::make_unique<I_imm_Inst>(
                    Op::addiu, Register::t9, Register::t9, 1));
        }
        baseStore.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::fp, Register::s2, Register::t8));
        baseStore.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::t9, Register::fp, 0));
        baseStore.push_back(std::make_unique<Label>(baseNoStore));
        assemblies.insert(assemblies.begin() + static_cast<long>(baseReturnStart),
                          std::make_move_iterator(baseStore.begin()),
                          std::make_move_iterator(baseStore.end()));

        std::vector<std::unique_ptr<Assembly>> lookup;
        lookup.push_back(std::make_unique<I_label_Inst>(
                Op::bltu, Register::s3, stateReg, cacheMiss));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t8, stateReg, 2));
        lookup.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::fp, Register::s2, Register::t8));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::lw, Register::t9, Register::fp, 0));
        lookup.push_back(std::make_unique<I_label_Inst>(
                Op::beqz, Register::t9, Register::none, cacheMiss));
        lookup.push_back(std::make_unique<I_label_Inst>(
                Op::blt, Register::t9, Register::zero, constantHit));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, Register::t8, Register::t9, -1));
        lookup.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::v0, accumulatorReg, Register::t8));
        lookup.push_back(std::make_unique<I_label_Inst>(
                Op::bltu, Register::s3, memoStartState, affineHitReturn));
        if (memoStartAccumulator == Register::zero) {
            lookup.push_back(std::make_unique<I_imm_Inst>(
                    Op::addiu, Register::t9, Register::v0, 1));
        } else {
            lookup.push_back(std::make_unique<R_Inst>(
                    Op::subu, Register::t9, Register::v0, memoStartAccumulator));
            lookup.push_back(std::make_unique<I_imm_Inst>(
                    Op::addiu, Register::t9, Register::t9, 1));
        }
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t8, memoStartState, 2));
        lookup.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::fp, Register::s2, Register::t8));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::t9, Register::fp, 0));
        lookup.push_back(std::make_unique<Label>(affineHitReturn));
        for (auto restore = preservedRegisters.rbegin(); restore != preservedRegisters.rend();
             ++restore) {
            lookup.push_back(std::make_unique<I_imm_Inst>(
                    Op::lw, restore->first, Register::sp, restore->second));
        }
        lookup.push_back(std::make_unique<R_Inst>(
                Op::jr, Register::none, Register::ra, Register::none));
        lookup.push_back(std::make_unique<Label>(constantHit));
        lookup.push_back(std::make_unique<R_Inst>(
                Op::subu, Register::v0, Register::zero, Register::t9));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, Register::v0, Register::v0, -1));
        lookup.push_back(std::make_unique<I_label_Inst>(
                Op::bltu, Register::s3, memoStartState, constantHitReturn));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t8, memoStartState, 2));
        lookup.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::fp, Register::s2, Register::t8));
        lookup.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::t9, Register::fp, 0));
        lookup.push_back(std::make_unique<Label>(constantHitReturn));
        for (auto restore = preservedRegisters.rbegin(); restore != preservedRegisters.rend();
             ++restore) {
            lookup.push_back(std::make_unique<I_imm_Inst>(
                    Op::lw, restore->first, Register::sp, restore->second));
        }
        lookup.push_back(std::make_unique<R_Inst>(
                Op::jr, Register::none, Register::ra, Register::none));
        lookup.push_back(std::make_unique<Label>(cacheMiss));
        assemblies.insert(assemblies.begin() + static_cast<long>(headerIndex + 1),
                          std::make_move_iterator(lookup.begin()),
                          std::make_move_iterator(lookup.end()));

        std::vector<std::unique_ptr<Assembly>> rememberStart;
        if (memoStartState == Register::a2) {
            rememberStart.push_back(std::make_unique<R_Inst>(
                    Op::move, Register::a2, stateReg, Register::none));
        }
        if (memoStartAccumulator == Register::a3) {
            rememberStart.push_back(std::make_unique<R_Inst>(
                    Op::move, Register::a3, accumulatorReg, Register::none));
        }
        assemblies.insert(assemblies.begin() + static_cast<long>(headerIndex),
                          std::make_move_iterator(rememberStart.begin()),
                          std::make_move_iterator(rememberStart.end()));

        std::vector<std::unique_ptr<Assembly>> allocation;
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s4, limitValueReg, Register::none));
        if (loopConstantReg != Register::none) {
            allocation.push_back(std::make_unique<I_imm_Inst>(
                    Op::li, Register::s5, Register::none, loopConstantValue));
        }
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::s6, Register::none, 1));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s2, Register::zero, Register::none));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s3, Register::zero, Register::none));
        allocation.push_back(std::make_unique<I_label_Inst>(
                Op::ble, limitValueReg, Register::zero, cacheBoundReady));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::s3, Register::none, maxMemoizedState));
        allocation.push_back(std::make_unique<I_label_Inst>(
                Op::bgt, limitValueReg, Register::s3, cacheBoundReady));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s3, limitValueReg, Register::none));
        allocation.push_back(std::make_unique<Label>(cacheBoundReady));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, Register::a0, Register::s3, 1));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::sll, Register::t9, Register::a0, 2));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::a0, Register::t9, Register::none));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::v0, Register::none, 9));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::syscall, Register::none, Register::none, Register::none));
        allocation.push_back(std::make_unique<I_label_Inst>(
                Op::ble, Register::v0, Register::zero, cacheAllocationFailed));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s2, Register::v0, Register::none));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::addu, Register::t9, Register::s2, Register::t9));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::t8, Register::s2, Register::none));
        allocation.push_back(std::make_unique<Label>(initLoop));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::zero, Register::t8, 0));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::addiu, Register::t8, Register::t8, 4));
        allocation.push_back(std::make_unique<I_label_Inst>(
                Op::bne, Register::t8, Register::t9, initLoop));
        allocation.push_back(std::make_unique<J_Inst>(Op::j, cacheDisabled));
        allocation.push_back(std::make_unique<Label>(cacheAllocationFailed));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s3, Register::zero, Register::none));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::a0, Register::none, 4));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::li, Register::v0, Register::none, 9));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::syscall, Register::none, Register::none, Register::none));
        allocation.push_back(std::make_unique<R_Inst>(
                Op::move, Register::s2, Register::v0, Register::none));
        allocation.push_back(std::make_unique<I_imm_Inst>(
                Op::sw, Register::zero, Register::s2, 0));
        allocation.push_back(std::make_unique<Label>(cacheDisabled));
        assemblies.insert(assemblies.begin() + static_cast<long>(limitStore + 1),
                          std::make_move_iterator(allocation.begin()),
                          std::make_move_iterator(allocation.end()));

        std::vector<size_t> cachedLimitLoads;
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *load = dynamic_cast<I_label_Inst *>(assemblies[index].get());
            if (!load || load->op != Op::lw || load->rt != Register::none
                || load->label.nameAndId != limitName) {
                continue;
            }
            const Register loaded = load->rs;
            for (size_t use = index + 1; use < assemblies.size(); ++use) {
                if (auto *label = dynamic_cast<Label *>(assemblies[use].get())) {
                    bool functionEntry = label->nameAndId == "main";
                    for (const auto &other: module.getFunctions()) {
                        functionEntry = functionEntry || label->nameAndId == other->getName();
                    }
                    if (functionEntry) {
                        break;
                    }
                }

                Assembly *assembly = assemblies[use].get();
                if (auto *inst = dynamic_cast<R_Inst *>(assembly)) {
                    if (inst->rs == loaded) {
                        inst->rs = Register::s4;
                    }
                    if (inst->rt == loaded) {
                        inst->rt = Register::s4;
                    }
                } else if (auto *inst = dynamic_cast<I_imm_Inst *>(assembly)) {
                    if (inst->rs == loaded) {
                        inst->rs = Register::s4;
                    }
                    if ((inst->op == Op::sw || inst->op == Op::sb) && inst->rt == loaded) {
                        inst->rt = Register::s4;
                    }
                } else if (auto *inst = dynamic_cast<I_label_Inst *>(assembly)) {
                    const bool branch = inst->op == Op::beq || inst->op == Op::bne
                                        || inst->op == Op::blt || inst->op == Op::ble
                                        || inst->op == Op::bge || inst->op == Op::bgt
                                        || inst->op == Op::bltu || inst->op == Op::beqz
                                        || inst->op == Op::bgtz;
                    if ((branch || inst->op == Op::sw || inst->op == Op::sb)
                        && inst->rs == loaded) {
                        inst->rs = Register::s4;
                    }
                    if ((branch || inst->op == Op::sw || inst->op == Op::sb
                         || inst->op == Op::lw || inst->op == Op::lbu || inst->op == Op::la)
                        && inst->rt == loaded) {
                        inst->rt = Register::s4;
                    }
                }
                if (writesReg(assembly, loaded)) {
                    break;
                }
            }
            cachedLimitLoads.push_back(index);
        }
        for (auto index = cachedLimitLoads.rbegin(); index != cachedLimitLoads.rend(); ++index) {
            assemblies.erase(assemblies.begin() + static_cast<long>(*index));
        }

        auto replaceRegisterReads = [](Assembly *assembly, Register from, Register to) {
            if (auto *inst = dynamic_cast<R_Inst *>(assembly)) {
                if (inst->rs == from) {
                    inst->rs = to;
                }
                if (inst->rt == from) {
                    inst->rt = to;
                }
            } else if (auto *inst = dynamic_cast<I_imm_Inst *>(assembly)) {
                if (inst->rs == from) {
                    inst->rs = to;
                }
                if ((inst->op == Op::sw || inst->op == Op::sb) && inst->rt == from) {
                    inst->rt = to;
                }
            } else if (auto *inst = dynamic_cast<I_label_Inst *>(assembly)) {
                const bool branch = inst->op == Op::beq || inst->op == Op::bne
                                    || inst->op == Op::blt || inst->op == Op::ble
                                    || inst->op == Op::bge || inst->op == Op::bgt
                                    || inst->op == Op::bltu || inst->op == Op::beqz
                                    || inst->op == Op::bgtz;
                if ((branch || inst->op == Op::sw || inst->op == Op::sb)
                    && inst->rs == from) {
                    inst->rs = to;
                }
                if ((branch || inst->op == Op::sw || inst->op == Op::sb
                     || inst->op == Op::lw || inst->op == Op::lbu || inst->op == Op::la)
                    && inst->rt == from) {
                    inst->rt = to;
                }
            }
        };

        if (loopConstantReg != Register::none) {
            for (size_t index = 0; index + 1 < assemblies.size(); ++index) {
                auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
                if (!constant || constant->op != Op::li || constant->rt != loopConstantReg
                    || constant->immediate != loopConstantValue) {
                    continue;
                }
                bool reachedBackedge = false;
                for (size_t use = index + 1; use < assemblies.size(); ++use) {
                    if (auto *jump = dynamic_cast<J_Inst *>(assemblies[use].get())) {
                        if (jump->op == Op::j && jump->label.nameAndId == hotLoopHeader) {
                            reachedBackedge = true;
                            break;
                        }
                    }
                    replaceRegisterReads(assemblies[use].get(), loopConstantReg, Register::s5);
                }
                if (reachedBackedge) {
                    assemblies.erase(assemblies.begin() + static_cast<long>(index));
                }
                break;
            }
        }

        size_t dynamicFunctionStart = assemblies.size();
        size_t dynamicFunctionEnd = assemblies.size();
        size_t dynamicHeader = assemblies.size();
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *label = dynamic_cast<Label *>(assemblies[index].get());
            if (!label) {
                continue;
            }
            if (label->nameAndId == function->getName()) {
                dynamicFunctionStart = index;
            } else if (label->nameAndId == headerName) {
                dynamicHeader = index;
            } else if (dynamicFunctionStart != assemblies.size()) {
                for (const auto &other: module.getFunctions()) {
                    if (label->nameAndId == other->getName()) {
                        dynamicFunctionEnd = index;
                        break;
                    }
                }
                if (dynamicFunctionEnd != assemblies.size()) {
                    break;
                }
            }
        }

        size_t oneDefinition = assemblies.size();
        for (size_t index = dynamicFunctionStart + 1; index < dynamicFunctionEnd; ++index) {
            auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
            if (constant && constant->op == Op::li && constant->rt == oneReg
                && constant->immediate == 1 && oneDefinition == assemblies.size()) {
                oneDefinition = index;
                continue;
            }
            if (!isPreservedStackAccess(assemblies[index].get())) {
                replaceRegisterReads(assemblies[index].get(), oneReg, Register::s6);
            }
        }
        if (oneDefinition != assemblies.size()) {
            assemblies.erase(assemblies.begin() + static_cast<long>(oneDefinition));
        }

        for (size_t index = 0; index + 1 < assemblies.size(); ++index) {
            auto *baseAfterShift = dynamic_cast<I_label_Inst *>(assemblies[index].get());
            auto *skipMemoLookup = dynamic_cast<J_Inst *>(assemblies[index + 1].get());
            if (baseAfterShift && skipMemoLookup && baseAfterShift->op == Op::beq
                && baseAfterShift->rs == stateReg
                && baseAfterShift->label.nameAndId == baseTrueName
                && skipMemoLookup->op == Op::j) {
                skipMemoLookup->label = Label(headerName);
            }
        }

        dynamicFunctionStart = assemblies.size();
        dynamicFunctionEnd = assemblies.size();
        dynamicHeader = assemblies.size();
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *label = dynamic_cast<Label *>(assemblies[index].get());
            if (!label) {
                continue;
            }
            if (label->nameAndId == function->getName()) {
                dynamicFunctionStart = index;
            } else if (label->nameAndId == headerName) {
                dynamicHeader = index;
            } else if (dynamicFunctionStart != assemblies.size()) {
                for (const auto &other: module.getFunctions()) {
                    if (label->nameAndId == other->getName()) {
                        dynamicFunctionEnd = index;
                        break;
                    }
                }
                if (dynamicFunctionEnd != assemblies.size()) {
                    break;
                }
            }
        }
        std::vector<size_t> deadEntryConstants;
        for (size_t index = dynamicFunctionStart + 1; index < dynamicHeader; ++index) {
            auto *constant = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
            if (!constant || constant->op != Op::li) {
                continue;
            }
            bool readBeforeOverwrite = false;
            for (size_t use = index + 1; use < dynamicFunctionEnd; ++use) {
                if (readsReg(assemblies[use].get(), constant->rt)) {
                    readBeforeOverwrite = true;
                    break;
                }
                if (writesReg(assemblies[use].get(), constant->rt)) {
                    break;
                }
            }
            if (!readBeforeOverwrite) {
                deadEntryConstants.push_back(index);
            }
        }
        for (auto index = deadEntryConstants.rbegin(); index != deadEntryConstants.rend(); ++index) {
            assemblies.erase(assemblies.begin() + static_cast<long>(*index));
        }

        size_t cleanupFunctionStart = assemblies.size();
        size_t cleanupFunctionEnd = assemblies.size();
        for (size_t index = 0; index < assemblies.size(); ++index) {
            auto *label = dynamic_cast<Label *>(assemblies[index].get());
            if (!label) {
                continue;
            }
            if (label->nameAndId == function->getName()) {
                cleanupFunctionStart = index;
                continue;
            }
            if (cleanupFunctionStart == assemblies.size()) {
                continue;
            }
            for (const auto &other: module.getFunctions()) {
                if (label->nameAndId == other->getName()) {
                    cleanupFunctionEnd = index;
                    break;
                }
            }
            if (cleanupFunctionEnd != assemblies.size()) {
                break;
            }
        }
        for (const auto &[reg, offset]: preservedRegisters) {
            bool needed = false;
            for (size_t index = cleanupFunctionStart + 1; index < cleanupFunctionEnd; ++index) {
                if (!isPreservedStackAccess(assemblies[index].get())
                    && mentions(assemblies[index].get(), reg)) {
                    needed = true;
                    break;
                }
            }
            if (needed) {
                continue;
            }
            for (size_t index = cleanupFunctionEnd; index-- > cleanupFunctionStart + 1;) {
                auto *memory = dynamic_cast<I_imm_Inst *>(assemblies[index].get());
                if (memory && (memory->op == Op::lw || memory->op == Op::sw)
                    && memory->rt == reg && memory->rs == Register::sp
                    && memory->immediate == offset) {
                    assemblies.erase(assemblies.begin() + static_cast<long>(index));
                    --cleanupFunctionEnd;
                }
            }
        }

        size_t inlineCall = assemblies.size();
        dynamicFunctionStart = assemblies.size();
        dynamicFunctionEnd = assemblies.size();
        for (size_t index = 0; index < assemblies.size(); ++index) {
            if (auto *call = dynamic_cast<J_Inst *>(assemblies[index].get())) {
                if (call->op == Op::jal && call->label.nameAndId == function->getName()) {
                    inlineCall = index;
                }
            }
            auto *label = dynamic_cast<Label *>(assemblies[index].get());
            if (!label) {
                continue;
            }
            if (label->nameAndId == function->getName()) {
                dynamicFunctionStart = index;
            } else if (dynamicFunctionStart != assemblies.size()) {
                for (const auto &other: module.getFunctions()) {
                    if (label->nameAndId == other->getName()) {
                        dynamicFunctionEnd = index;
                        break;
                    }
                }
                if (dynamicFunctionEnd != assemblies.size()) {
                    break;
                }
            }
        }
        if (inlineCall != assemblies.size() && dynamicFunctionStart != assemblies.size()
            && inlineCall < dynamicFunctionStart) {
            const Label continuation("__mips_inline_return_" + std::to_string(id));
            std::vector<std::unique_ptr<Assembly>> inlined;
            inlined.reserve(dynamicFunctionEnd - dynamicFunctionStart);
            for (size_t index = dynamicFunctionStart + 1; index < dynamicFunctionEnd; ++index) {
                if (auto *returnInst = dynamic_cast<R_Inst *>(assemblies[index].get());
                    returnInst && returnInst->op == Op::jr && returnInst->rs == Register::ra) {
                    inlined.push_back(std::make_unique<J_Inst>(Op::j, continuation));
                } else {
                    inlined.push_back(std::move(assemblies[index]));
                }
            }
            assemblies.erase(assemblies.begin() + static_cast<long>(dynamicFunctionStart),
                             assemblies.begin() + static_cast<long>(dynamicFunctionEnd));
            assemblies.erase(assemblies.begin() + static_cast<long>(inlineCall));
            inlined.push_back(std::make_unique<Label>(continuation));
            assemblies.insert(assemblies.begin() + static_cast<long>(inlineCall),
                              std::make_move_iterator(inlined.begin()),
                              std::make_move_iterator(inlined.end()));
        }
        changed = true;
        break;
    }
    return changed;
}

bool isReadBeforeOverwrite(size_t start, Register reg) {
    if (!validReg(reg)) {
        return false;
    }
    for (size_t i = start; i < assemblies.size(); ++i) {
        const Assembly *assembly = assemblies[i].get();
        if (readsReg(assembly, reg)) {
            return true;
        }
        if (writesReg(assembly, reg)) {
            return false;
        }
        if (isControlBoundary(assembly)) {
            return true;
        }
    }
    return false;
}

bool mergeMoveIntoNextR(size_t index, bool requireRsUse, bool requireRtUse) {
    auto move = dynamic_cast<R_Inst *>(assemblies[index].get());
    auto cal = dynamic_cast<R_Inst *>(assemblies[index + 1].get());
    if (!move || !cal || move->op != Op::move) {
        return false;
    }
    if (requireRsUse && cal->rs != move->rd) {
        return false;
    }
    if (requireRtUse && cal->rt != move->rd) {
        return false;
    }

    const bool used = cal->rs == move->rd || cal->rt == move->rd;
    if (!used) {
        return false;
    }
    if (!writesReg(*cal, move->rd) && isReadBeforeOverwrite(index + 2, move->rd)) {
        return false;
    }

    if (cal->rs == move->rd) {
        cal->rs = move->rs;
    }
    if (cal->rt == move->rd) {
        cal->rt = move->rs;
    }

    assemblies.erase(assemblies.begin() + index);
    return true;
}

} // namespace

// merge immediate instructions
bool MIPS::allMergeLi_R() {
    // li   $t1 1
    // addu $t2 $t0 $t1
    // ------------------
    // addiu $t2 $t0 1
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto r = dynamic_cast<R_Inst *>(inst2);
            if (!li || !r) {
                ++i;
                continue;
            }

            constexpr int MAX_16_BIT = (1 << 15) - 1;
            constexpr int NEG_MIN_16_BIT = -(1 << 15);
            if (li->immediate > MAX_16_BIT || li->immediate < NEG_MIN_16_BIT) {
                ++i;
                continue;
            }

            if (r->rt == li->rt && r->rs != li->rt && rOp_ImmOp(r->op) != Op::none
                && (r->rd == li->rt || !isReadBeforeOverwrite(i + 2, li->rt))) {
                *assem2 = mergeLi_R(*li, *r);
                assemblies.erase(assem1);
                flag = true;
                continue;
            }
        }
        ++i;
    }
    return flag;
}

bool MIPS::allMergeLi_Move() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto inst1 = dynamic_cast<Instruction *>(assem1->get());
        auto inst2 = dynamic_cast<Instruction *>(assem2->get());

        if (inst1 && inst1->op == Op::li && inst2 && inst2->op == Op::move) {
            auto li = dynamic_cast<I_imm_Inst *>(inst1);
            auto move = dynamic_cast<R_Inst *>(inst2);
            if (li && move && li->rt == move->rs
                && (move->rd == li->rt || !isReadBeforeOverwrite(i + 2, li->rt))) {
                li->rt = move->rd;
                assemblies.erase(assem2);
                flag = true;
                continue;
            }
        }
        ++i;
    }

    return flag;
}

// Load
bool MIPS::allMergeMove_R_rs() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        if (mergeMoveIntoNextR(i, true, false)) {
            flag = true;
            continue;
        }
        ++i;
    }

    return flag;
}

bool MIPS::allMergeMove_R_rt() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        if (mergeMoveIntoNextR(i, false, true)) {
            flag = true;
            continue;
        }
        ++i;
    }

    return flag;
}

bool MIPS::allMergeR_Move() {
    bool flag = false;
    for (size_t i = 0; i + 1 < assemblies.size();) {
        auto assem1 = assemblies.begin() + i;
        auto assem2 = assem1 + 1;

        auto move = dynamic_cast<R_Inst *>(assem2->get());
        if (move && move->op == Op::move) {
            if (auto cal = dynamic_cast<R_Inst *>(assem1->get());
                cal && cal->op != Op::move && writesReg(*cal, cal->rd)
                && cal->rd == move->rs
                && (move->rd == cal->rd || !isReadBeforeOverwrite(i + 2, cal->rd))) {
                cal->rd = move->rd;
                assemblies.erase(assem2);
                flag = true;
                continue;
            }
            if (auto immediate = dynamic_cast<I_imm_Inst *>(assem1->get());
                immediate && writesReg(*immediate, immediate->rt)
                && immediate->rt == move->rs
                && (move->rd == immediate->rt
                    || !isReadBeforeOverwrite(i + 2, immediate->rt))) {
                immediate->rt = move->rd;
                assemblies.erase(assem2);
                flag = true;
                continue;
            }
        }
        ++i;
    }

    return flag;
}

void MIPS::irToMips(const IR::Inst &inst) {
    beginInstruction(inst);
    switch (inst.op) {
        case IR::Op::Empty:
            break;
        case IR::Op::InStack:
            InStack(inst);
            break;
        case IR::Op::OutStack:
            OutStack(inst);
            break;
        case IR::Op::Store:
            Store(inst);
            break;
        case IR::Op::StoreDynamic:
            StoreDynamic(inst);
            break;
        case IR::Op::Add:
            Add(inst);
            break;
        case IR::Op::Sub:
            Sub(inst);
            break;
        case IR::Op::Mul:
            Mul(inst);
            break;
        case IR::Op::Div:
            Div(inst);
            break;
        case IR::Op::Mod:
            Mod(inst);
            break;
        case IR::Op::And:
            And(inst);
            break;
        case IR::Op::Or:
            Or(inst);
            break;
        case IR::Op::Neg:
            Neg(inst);
            break;
        case IR::Op::LoadImd:
            LoadImd(inst);
            break;
        case IR::Op::GetInt:
            GetInt(inst);
            break;
        case IR::Op::GetChar:
            GetChar(inst);
            break;
        case IR::Op::GetString:
            GetString(inst);
            break;
        case IR::Op::PrintInt:
            PrintInt(inst);
            break;
        case IR::Op::PrintChar:
            PrintChar(inst);
            break;
        case IR::Op::PrintStr:
            PrintStr(inst);
            break;
        case IR::Op::Alloca:
            Alloca(inst);
            break;
        case IR::Op::Load:
            Load(inst);
            break;
        case IR::Op::LoadPtr:
            LoadPtr(inst);
            break;
        case IR::Op::Br:
            Br(inst);
            break;
        case IR::Op::Bif0:
            Bif0(inst);
            break;
        case IR::Op::Call:
            Call(inst);
            break;
        case IR::Op::PushParam:
            PushParam(inst);
            break;
        case IR::Op::Ret:
            Ret(inst);
            break;
        case IR::Op::RetMain:
            RetMain(inst);
            break;
        case IR::Op::NewMove:
            NewMove(inst);
            break;
        case IR::Op::Leq:
            Leq(inst);
            break;
        case IR::Op::Lss:
            Lss(inst);
            break;
        case IR::Op::Geq:
            Geq(inst);
            break;
        case IR::Op::Gre:
            Gre(inst);
            break;
        case IR::Op::Eql:
            Eql(inst);
            break;
        case IR::Op::Neq:
            Neq(inst);
            break;
        case IR::Op::Bif1:
            Bif1(inst);
            break;
        case IR::Op::LoadDynamic:
            LoadDynamic(inst);
            break;
        case IR::Op::MulImd:
            MulImd(inst);
            break;
        case IR::Op::Mult4:
            Mult4(inst);
            break;
        case IR::Op::PushAddressParam:
            PushAddressParam(inst);
            break;
        case IR::Op::Not:
            Not(inst);
            break;
        case IR::Op::Parameter:
            Load(inst);
            break;
        case IR::Op::Phi:
            Error::raise("Phi must be lowered before MIPS generation");
            break;
        default:
            Error::raise("Bad IR Op in MIPS gen");
    }
    endInstruction();
}
