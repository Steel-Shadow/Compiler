#include "middle/Optimize.h"

#include "middle/Analysis.h"
#include "middle/IRUtils.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IR {
namespace {

bool hasHardTerminator(const BasicBlock &block) {
    return std::any_of(block.instructions.begin(), block.instructions.end(), [](const Inst &inst) {
        return inst.op == Op::Br || inst.op == Op::Ret || inst.op == Op::RetMain;
    });
}

void makeFallthroughExplicit(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    for (size_t block = 0; block + 1 < blocks.size(); ++block) {
        if (!hasHardTerminator(*blocks[block])) {
            blocks[block]->instructions.emplace_back(
                    Op::Br, nullptr, blocks[block + 1]->label.clone(), nullptr);
        }
    }
}

bool retarget(Inst &inst, const std::string &from, const Label &to) {
    std::unique_ptr<Element> *slot = nullptr;
    if (inst.op == Op::Br) {
        slot = &inst.arg1;
    } else if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
        slot = &inst.arg2;
    }
    const auto *target = slot ? asLabel(*slot) : nullptr;
    if (!target || target->nameAndId != from) {
        return false;
    }
    *slot = to.clone();
    return true;
}

bool removeUnreachable(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()
        || std::all_of(cfg.reachable.begin(), cfg.reachable.end(), [](bool value) { return value; })) {
        return false;
    }
    BasicBlocks kept;
    kept.reserve(blocks.size());
    for (size_t block = 0; block < blocks.size(); ++block) {
        if (block < cfg.reachable.size() && cfg.reachable[block]) {
            kept.push_back(std::move(blocks[block]));
        }
    }
    blocks = std::move(kept);
    simplifyPhiNodes(function);
    return true;
}

bool isJumpOnly(const BasicBlock &block, std::string &target) {
    const Inst *jump = nullptr;
    for (const auto &inst: block.instructions) {
        if (inst.isScopeMarker()) {
            return false;
        }
        if (inst.op != Op::Br || jump) {
            return false;
        }
        jump = &inst;
    }
    const auto *label = jump ? asLabel(jump->arg1) : nullptr;
    if (!label || label->nameAndId == block.label.nameAndId) {
        return false;
    }
    target = label->nameAndId;
    return true;
}

} // namespace

bool normalizeBasicBlocks(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    BasicBlocks normalized;
    normalized.reserve(blocks.size());
    bool changed = false;

    for (auto &source: blocks) {
        auto current = std::make_unique<BasicBlock>(source->label.nameAndId, true);
        for (size_t index = 0; index < source->instructions.size(); ++index) {
            const bool splitAfter = (source->instructions[index].op == Op::Bif0
                                     || source->instructions[index].op == Op::Bif1)
                                    && index + 1 < source->instructions.size();
            current->instructions.push_back(std::move(source->instructions[index]));
            if (!splitAfter) {
                continue;
            }

            normalized.push_back(std::move(current));
            current = std::make_unique<BasicBlock>(function.getName() + "_fallthrough");
            changed = true;
        }
        normalized.push_back(std::move(current));
    }

    blocks = std::move(normalized);
    return changed;
}

bool simplifyControlFlow(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.empty()) {
        return false;
    }
    makeFallthroughExplicit(function);
    bool changed = false;

    for (auto &block: blocks) {
        std::string finalTarget;
        for (auto reverse = block->instructions.rbegin(); reverse != block->instructions.rend(); ++reverse) {
            if (reverse->isScopeMarker()) {
                continue;
            }
            if (reverse->op == Op::Br) {
                const auto *target = asLabel(reverse->arg1);
                finalTarget = target ? target->nameAndId : std::string{};
            }
            break;
        }
        if (finalTarget.empty()) {
            continue;
        }
        auto &instructions = block->instructions;
        for (size_t index = 0; index < instructions.size();) {
            const auto *target = (instructions[index].op == Op::Bif0
                                  || instructions[index].op == Op::Bif1)
                                         ? asLabel(instructions[index].arg2)
                                         : nullptr;
            if (target && target->nameAndId == finalTarget) {
                instructions.erase(instructions.begin() + static_cast<long>(index));
                changed = true;
            } else {
                ++index;
            }
        }
    }

    std::unordered_map<std::string, size_t> labels;
    for (size_t block = 0; block < blocks.size(); ++block) {
        labels.emplace(blocks[block]->label.nameAndId, block);
    }
    std::unordered_map<std::string, std::string> jumpTargets;
    for (size_t block = 1; block < blocks.size(); ++block) {
        std::string target;
        if (isJumpOnly(*blocks[block], target)) {
            jumpTargets.emplace(blocks[block]->label.nameAndId, std::move(target));
        }
    }
    auto resolve = [&](std::string target) {
        std::unordered_set<std::string> visited;
        while (visited.insert(target).second) {
            auto jump = jumpTargets.find(target);
            if (jump == jumpTargets.end()) {
                break;
            }
            auto destination = labels.find(jump->second);
            if (destination == labels.end()) {
                break;
            }
            const auto &instructions = blocks[destination->second]->instructions;
            if (!instructions.empty() && instructions.front().op == Op::Phi) {
                break;
            }
            target = jump->second;
        }
        return target;
    };
    for (auto &block: blocks) {
        for (auto &inst: block->instructions) {
            std::unique_ptr<Element> *slot = nullptr;
            if (inst.op == Op::Br) {
                slot = &inst.arg1;
            } else if (inst.op == Op::Bif0 || inst.op == Op::Bif1) {
                slot = &inst.arg2;
            }
            const auto *target = slot ? asLabel(*slot) : nullptr;
            if (!target || jumpTargets.find(target->nameAndId) == jumpTargets.end()) {
                continue;
            }
            const std::string destination = resolve(target->nameAndId);
            if (destination == target->nameAndId) {
                continue;
            }
            auto destinationBlock = labels.find(destination);
            if (destinationBlock != labels.end()) {
                *slot = blocks[destinationBlock->second]->label.clone();
                changed = true;
            }
        }
    }
    return removeUnreachable(function) || changed;
}

bool canonicalizeLoops(Function &function) {
    const auto cfg = buildControlFlowGraph(function);
    const auto loops = collectNaturalLoops(cfg);
    auto &blocks = function.getMutableBasicBlocks();
    for (const auto &[header, loop]: loops) {
        if (header == 0 || header >= blocks.size()) {
            continue;
        }
        std::vector<size_t> outside;
        for (size_t predecessor: cfg.predecessors[header]) {
            if (loop.find(predecessor) == loop.end()) {
                outside.push_back(predecessor);
            }
        }
        if (outside.empty()
            || (outside.size() == 1 && cfg.successors[outside.front()].size() == 1)) {
            continue;
        }

        const std::string oldHeader = blocks[header]->label.nameAndId;
        auto preheader = std::make_unique<BasicBlock>(oldHeader + "_preheader");
        const Label preheaderLabel = preheader->label;

        std::unordered_set<std::string> outsideLabels;
        for (size_t predecessor: outside) {
            outsideLabels.insert(blocks[predecessor]->label.nameAndId);
        }
        int tempId = nextTempId(function);
        for (auto &phi: blocks[header]->instructions) {
            if (phi.op != Op::Phi) {
                break;
            }
            std::vector<size_t> outsideIncoming;
            for (size_t index = 0; index < phi.phiIncoming.size(); ++index) {
                if (outsideLabels.count(phi.phiIncoming[index].predecessor) != 0) {
                    outsideIncoming.push_back(index);
                }
            }
            if (outsideIncoming.size() == 1) {
                phi.phiIncoming[outsideIncoming.front()].predecessor =
                        preheaderLabel.nameAndId;
                continue;
            }
            const auto *result = dynamic_cast<const Temp *>(phi.res.get());
            if (outsideIncoming.empty() || !result) {
                continue;
            }

            const Temp merged(tempId++, result->type);
            Inst preheaderPhi(Op::Phi, merged.clone(), nullptr, nullptr);
            for (size_t index: outsideIncoming) {
                preheaderPhi.addPhiIncoming(
                        phi.phiIncoming[index].predecessor,
                        phi.phiIncoming[index].value->clone());
            }
            phi.phiIncoming.erase(
                    std::remove_if(
                            phi.phiIncoming.begin(), phi.phiIncoming.end(),
                            [&](const PhiIncoming &incoming) {
                                return outsideLabels.count(incoming.predecessor) != 0;
                            }),
                    phi.phiIncoming.end());
            phi.addPhiIncoming(preheaderLabel.nameAndId, merged.clone());
            preheader->instructions.push_back(std::move(preheaderPhi));
        }
        preheader->instructions.emplace_back(
                Op::Br, nullptr, blocks[header]->label.clone(), nullptr);

        for (size_t predecessor: outside) {
            for (auto &inst: blocks[predecessor]->instructions) {
                retarget(inst, oldHeader, preheaderLabel);
            }
        }
        blocks.insert(blocks.begin() + static_cast<long>(header), std::move(preheader));
        return true;
    }
    return false;
}

bool orderBasicBlocks(Function &function) {
    auto &blocks = function.getMutableBasicBlocks();
    if (blocks.size() < 2) {
        return false;
    }
    makeFallthroughExplicit(function);
    const auto cfg = buildControlFlowGraph(function);
    const auto loopDepth = computeLoopDepths(cfg);
    std::vector<bool> placed(blocks.size(), false);
    std::vector<size_t> order;
    order.reserve(blocks.size());

    auto appendTrace = [&](size_t start) {
        size_t block = start;
        while (!placed[block]) {
            placed[block] = true;
            order.push_back(block);
            size_t best = ControlFlowGraph::NoBlock;
            for (size_t successor: cfg.successors[block]) {
                if (placed[successor]) {
                    continue;
                }
                if (best == ControlFlowGraph::NoBlock
                    || loopDepth[successor] > loopDepth[best]
                    || (loopDepth[successor] == loopDepth[best] && successor < best)) {
                    best = successor;
                }
            }
            if (best == ControlFlowGraph::NoBlock) {
                break;
            }
            block = best;
        }
    };
    appendTrace(0);
    while (order.size() < blocks.size()) {
        size_t best = ControlFlowGraph::NoBlock;
        for (size_t block = 0; block < blocks.size(); ++block) {
            if (!placed[block]
                && (best == ControlFlowGraph::NoBlock
                    || loopDepth[block] > loopDepth[best])) {
                best = block;
            }
        }
        appendTrace(best);
    }

    bool changed = false;
    for (size_t index = 0; index < order.size(); ++index) {
        changed = changed || order[index] != index;
    }
    if (changed) {
        BasicBlocks reordered;
        reordered.reserve(blocks.size());
        for (size_t block: order) {
            reordered.push_back(std::move(blocks[block]));
        }
        blocks = std::move(reordered);
    }

    for (size_t block = 0; block + 1 < blocks.size(); ++block) {
        auto &instructions = blocks[block]->instructions;
        for (size_t reverse = instructions.size(); reverse-- > 0;) {
            if (instructions[reverse].isScopeMarker()) {
                continue;
            }
            const auto *target = instructions[reverse].op == Op::Br
                                         ? asLabel(instructions[reverse].arg1)
                                         : nullptr;
            if (target && target->nameAndId == blocks[block + 1]->label.nameAndId) {
                instructions.erase(instructions.begin() + static_cast<long>(reverse));
                changed = true;
            }
            break;
        }
    }
    return changed;
}

} // namespace IR
