#include "middle/Optimize.h"

#include "middle/Analysis.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IR {
namespace {

struct Definition {
    size_t block{};
    size_t instruction{};
};

int nextTempId(const Function &function) {
    int next = 0;
    for (const auto &block: function.getBasicBlocks()) {
        for (const auto &inst: block->instructions) {
            for (const auto &operand: inst.operands()) {
                const auto *temp = dynamic_cast<const Temp *>(operand.value);
                if (temp && temp->id >= next) {
                    next = temp->id + 1;
                }
            }
        }
    }
    return next;
}

std::unordered_map<size_t, std::unordered_set<size_t>> naturalLoops(
        const ControlFlowGraph &cfg) {
    std::unordered_map<size_t, std::unordered_set<size_t>> loops;
    for (size_t latch = 0; latch < cfg.successors.size(); ++latch) {
        for (size_t header: cfg.successors[latch]) {
            if (!cfg.dominates(header, latch)) {
                continue;
            }
            auto &loop = loops[header];
            loop.insert(header);
            loop.insert(latch);
            std::vector<size_t> work{latch};
            while (!work.empty()) {
                const size_t block = work.back();
                work.pop_back();
                for (size_t predecessor: cfg.predecessors[block]) {
                    if (loop.insert(predecessor).second && predecessor != header) {
                        work.push_back(predecessor);
                    }
                }
            }
        }
    }
    return loops;
}

std::optional<int> knownConstant(
        const Element *element,
        const std::unordered_map<int, int> &constants) {
    if (const auto *constant = dynamic_cast<const ConstVal *>(element)) {
        return constant->value;
    }
    const auto *temp = dynamic_cast<const Temp *>(element);
    auto value = temp ? constants.find(temp->id) : constants.end();
    return value == constants.end() ? std::nullopt
                                    : std::optional<int>(value->second);
}

bool isPowerOfTwoMagnitude(int value) {
    const uint32_t magnitude = value < 0
                                       ? 0U - static_cast<uint32_t>(value)
                                       : static_cast<uint32_t>(value);
    return magnitude != 0 && (magnitude & (magnitude - 1)) == 0;
}

int wrappingMultiply(int lhs, int rhs) {
    return static_cast<int32_t>(static_cast<uint32_t>(lhs)
                                * static_cast<uint32_t>(rhs));
}

bool sameTemp(const Element *element, int id) {
    const auto *temp = dynamic_cast<const Temp *>(element);
    return temp && temp->id == id;
}

} // namespace

bool reduceInductionVariableStrength(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto loops = naturalLoops(cfg);
    if (loops.empty()) {
        return false;
    }

    std::unordered_map<int, Definition> definitions;
    std::unordered_map<int, int> constants;
    const auto &blocks = function.getBasicBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        for (size_t index = 0; index < blocks[block]->instructions.size(); ++index) {
            const auto &inst = blocks[block]->instructions[index];
            auto definition = definedTemp(inst);
            if (!definition) {
                continue;
            }
            definitions[*definition] = {block, index};
            const auto *constant = inst.op == Op::LoadImd
                                           ? dynamic_cast<const ConstVal *>(inst.arg1.get())
                                           : nullptr;
            if (constant) {
                constants[*definition] = constant->value;
            }
        }
    }

    for (const auto &[header, loop]: loops) {
        std::vector<size_t> outsidePredecessors;
        std::vector<size_t> latches;
        for (size_t predecessor: cfg.predecessors[header]) {
            (loop.find(predecessor) == loop.end() ? outsidePredecessors : latches)
                    .push_back(predecessor);
        }
        if (outsidePredecessors.size() != 1 || latches.size() != 1) {
            continue;
        }
        const size_t preheader = outsidePredecessors.front();
        const size_t latch = latches.front();
        if (cfg.successors[preheader].size() != 1) {
            continue;
        }

        for (const auto &phi: blocks[header]->instructions) {
            if (phi.op != Op::Phi) {
                break;
            }
            const auto *induction = dynamic_cast<const Temp *>(phi.res.get());
            if (!induction || induction->id < 0 || induction->type != Type::Int
                || phi.phiIncoming.size() != 2) {
                continue;
            }

            const Element *initial = nullptr;
            const Temp *backedge = nullptr;
            for (const auto &incoming: phi.phiIncoming) {
                if (incoming.predecessor == blocks[preheader]->label.nameAndId) {
                    initial = incoming.value.get();
                } else if (incoming.predecessor == blocks[latch]->label.nameAndId) {
                    backedge = dynamic_cast<const Temp *>(incoming.value.get());
                }
            }
            auto updateDefinition = backedge ? definitions.find(backedge->id) : definitions.end();
            if (!initial || !backedge || updateDefinition == definitions.end()
                || updateDefinition->second.block != latch) {
                continue;
            }

            const auto &update = blocks[latch]->instructions[updateDefinition->second.instruction];
            std::optional<int> step;
            if (update.op == Op::Add) {
                if (sameTemp(update.arg1.get(), induction->id)) {
                    step = knownConstant(update.arg2.get(), constants);
                } else if (sameTemp(update.arg2.get(), induction->id)) {
                    step = knownConstant(update.arg1.get(), constants);
                }
            } else if (update.op == Op::Sub && sameTemp(update.arg1.get(), induction->id)) {
                auto magnitude = knownConstant(update.arg2.get(), constants);
                if (magnitude) {
                    step = static_cast<int32_t>(0U - static_cast<uint32_t>(*magnitude));
                }
            }
            if (!step || *step == 0) {
                continue;
            }

            for (size_t candidateBlock: loop) {
                const auto &instructions = blocks[candidateBlock]->instructions;
                for (size_t candidateIndex = 0; candidateIndex < instructions.size();
                     ++candidateIndex) {
                    const auto &candidate = instructions[candidateIndex];
                    const auto *product = dynamic_cast<const Temp *>(candidate.res.get());
                    if (!product || product->id < 0 || product->type != Type::Int) {
                        continue;
                    }

                    std::optional<int> factor;
                    if (candidate.op == Op::Mul || candidate.op == Op::MulImd) {
                        if (sameTemp(candidate.arg1.get(), induction->id)) {
                            factor = knownConstant(candidate.arg2.get(), constants);
                        } else if (candidate.op == Op::Mul
                                   && sameTemp(candidate.arg2.get(), induction->id)) {
                            factor = knownConstant(candidate.arg1.get(), constants);
                        }
                    }
                    if (!factor || *factor == 0 || *factor == 1 || *factor == -1
                        || isPowerOfTwoMagnitude(*factor)) {
                        continue;
                    }

                    const int nextId = nextTempId(function);
                    const Temp initialProduct(nextId, Type::Int);
                    const Temp strideProduct(nextId + 1, Type::Int);
                    const Temp derivedPhi(nextId + 2, Type::Int);
                    const Temp derivedNext(nextId + 3, Type::Int);
                    const auto initialConstant = knownConstant(initial, constants);
                    auto initialValue = initial->clone();

                    function.replaceAllUsesWith(*product, derivedPhi);
                    auto &mutableBlocks = function.getMutableBasicBlocks();
                    auto &candidateInstructions = mutableBlocks[candidateBlock]->instructions;
                    candidateInstructions.erase(
                            candidateInstructions.begin() + static_cast<long>(candidateIndex));

                    auto &preheaderInstructions = mutableBlocks[preheader]->instructions;
                    auto preheaderTerminator = std::find_if(
                            preheaderInstructions.begin(), preheaderInstructions.end(),
                            [](const Inst &inst) { return inst.isTerminator(); });
                    std::vector<Inst> setup;
                    if (initialConstant) {
                        setup.emplace_back(
                                Op::LoadImd,
                                initialProduct.clone(),
                                std::make_unique<ConstVal>(
                                        wrappingMultiply(*initialConstant, *factor), Type::Int),
                                nullptr);
                    } else {
                        setup.emplace_back(
                                Op::MulImd,
                                initialProduct.clone(),
                                std::move(initialValue),
                                std::make_unique<ConstVal>(*factor, Type::Int));
                    }
                    setup.emplace_back(
                            Op::LoadImd,
                            strideProduct.clone(),
                            std::make_unique<ConstVal>(
                                    wrappingMultiply(*step, *factor), Type::Int),
                            nullptr);
                    preheaderInstructions.insert(
                            preheaderTerminator,
                            std::make_move_iterator(setup.begin()),
                            std::make_move_iterator(setup.end()));

                    Inst derived(Op::Phi, derivedPhi.clone(), nullptr, nullptr);
                    derived.addPhiIncoming(
                            mutableBlocks[preheader]->label.nameAndId,
                            initialProduct.clone());
                    derived.addPhiIncoming(
                            mutableBlocks[latch]->label.nameAndId,
                            derivedNext.clone());
                    auto &headerInstructions = mutableBlocks[header]->instructions;
                    auto afterPhis = std::find_if(
                            headerInstructions.begin(), headerInstructions.end(),
                            [](const Inst &inst) { return inst.op != Op::Phi; });
                    headerInstructions.insert(afterPhis, std::move(derived));

                    auto &latchInstructions = mutableBlocks[latch]->instructions;
                    auto latchTerminator = std::find_if(
                            latchInstructions.begin(), latchInstructions.end(),
                            [](const Inst &inst) { return inst.isTerminator(); });
                    latchInstructions.insert(
                            latchTerminator,
                            Inst(Op::Add,
                                 derivedNext.clone(),
                                 derivedPhi.clone(),
                                 strideProduct.clone()));
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace IR
