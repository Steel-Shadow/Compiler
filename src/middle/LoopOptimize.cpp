#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace IR {
namespace {

bool isPowerOfTwoMagnitude(int value) {
    const uint32_t magnitude = value < 0
                                       ? 0U - static_cast<uint32_t>(value)
                                       : static_cast<uint32_t>(value);
    return magnitude != 0 && (magnitude & (magnitude - 1)) == 0;
}

} // namespace

bool reduceInductionVariableStrength(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto loops = collectNaturalLoops(cfg);
    if (loops.empty()) {
        return false;
    }

    const auto definitions = collectTempDefinitions(function);
    const auto constants = collectImmediateConstants(function);
    const auto &blocks = function.getBasicBlocks();

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
                    step = constantValue(update.arg2.get(), constants);
                } else if (sameTemp(update.arg2.get(), induction->id)) {
                    step = constantValue(update.arg1.get(), constants);
                }
            } else if (update.op == Op::Sub && sameTemp(update.arg1.get(), induction->id)) {
                auto magnitude = constantValue(update.arg2.get(), constants);
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
                            factor = constantValue(candidate.arg2.get(), constants);
                        } else if (candidate.op == Op::Mul
                                   && sameTemp(candidate.arg2.get(), induction->id)) {
                            factor = constantValue(candidate.arg1.get(), constants);
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
                    const auto initialConstant = constantValue(initial, constants);
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
