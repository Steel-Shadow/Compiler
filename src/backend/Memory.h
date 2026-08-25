//
// Created by Steel_Shadow on 2023/11/15.
//

#ifndef STACKMEMORY_H
#define STACKMEMORY_H

#include "middle/IR.h"

#include <stack>
#include <unordered_map>

namespace MIPS {
constexpr int gp_init = 0x1000'8000;
constexpr int data_segment = 0x1001'0000;

int getStackOffset(const IR::Var *var);

// Stack layout at a non-main function entry (offsets are relative to $sp):
//
//   positive offsets: stack parameters
//                0: first parameter
//               -4: saved $ra slot
//          -8..-32: caller-save slots for graph-colored $t0-$t6
//         -36..-64: callee-save slots for graph-colored $s0-$s7
//        below -64: local variables and spilled temporaries
//
// The fixed call area keeps parameter and save-slot offsets independent of the
// actual colors used by either caller or callee.
//
// when generating MIPS form IR,
// if we get inst.op == InStack/outStack (BigForStmt IfStmt BlockStmt),
// push/pop curOffset into/from stack<int> offsetStack
namespace StackMemory {
// clear when generating MIPS for a new Function
extern std::unordered_map<IR::Var, int> varToOffset;
extern std::unordered_map<int, int> tempToOffset;

extern int curOffset;
extern std::stack<int> offsetStack;
} // namespace StackMemory
} // namespace MIPS

#endif //STACKMEMORY_H
