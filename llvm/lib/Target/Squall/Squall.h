//===-- Squall.h - Top-level interface for Squall ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALL_H
#define LLVM_LIB_TARGET_SQUALL_SQUALL_H

#include "llvm/Support/CodeGen.h"

namespace llvm {
class FunctionPass;
class ModulePass;
class PassRegistry;
class SquallTargetMachine;

FunctionPass *createSquallISelDag(SquallTargetMachine &TM,
                                  CodeGenOptLevel OptLevel);
ModulePass *createSquallCheckIRPass();
ModulePass *createSquallLowerMemoryPass();
ModulePass *createSquallLowerFloatPass();
ModulePass *createSquallLowerAllocaPass();
FunctionPass *createSquallForkJoinPass();

void initializeSquallDAGToDAGISelLegacyPass(PassRegistry &);
void initializeSquallAsmPrinterPass(PassRegistry &);
void initializeSquallCheckIRPass(PassRegistry &);
void initializeSquallLowerMemoryPass(PassRegistry &);
void initializeSquallLowerFloatPass(PassRegistry &);
void initializeSquallLowerAllocaPass(PassRegistry &);
void initializeSquallForkJoinPass(PassRegistry &);

namespace Squall {
/// Comparison kinds of the SETCC pseudo.
enum CondCode { EQ, NE, LT, LE, GT, GE, ULT, ULE, UGT, UGE };

/// Fixed words of every activation frame, relative to its frame pointer.
enum FrameWord : unsigned {
  ReturnTagWord = 0,   ///< Packed continuation tag written by the caller.
  ScratchWord = 1,     ///< Temporary used inside one pseudo expansion.
  LaneScratchWord = 2, ///< The same, in the second of two parallel lanes.
  FirstRegWord = 3,    ///< Word of register S0.
};

/// The number of argument registers; argument I arrives in S<I>.
constexpr unsigned NumArgRegs = 16;

/// Frame-relative and absolute slot fields are unsigned 11-bit word numbers.
constexpr unsigned MaxSlotWord = 2047;

/// With -squall-fork-join, a lane that calls a function joins on its own
/// absolute word, one of this many reserved before the globals; other lanes
/// share one join word. Joins need words that start empty, and frame words
/// start as constants, which a match can never empty.
constexpr unsigned NumCallJoinWords = 32;
/// Whether -squall-fork-join is given.
bool isForkJoinEnabled();
} // namespace Squall
} // namespace llvm

#endif
