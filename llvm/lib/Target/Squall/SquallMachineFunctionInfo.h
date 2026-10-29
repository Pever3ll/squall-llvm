//===-- SquallMachineFunctionInfo.h - Squall frame layout -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALLMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_SQUALL_SQUALLMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

/// Frame layout decided by frame lowering, in 64-bit words:
///
///   [0, FirstRegWord)         return tag and expansion scratch
///   [FirstRegWord, ObjectBase) allocated registers S0..Sn
///   [ObjectBase, FrameWords)  frame objects (allocas and spill slots)
///
/// A callee's frame pointer is the caller's plus FrameWords.
class SquallMachineFunctionInfo : public MachineFunctionInfo {
  unsigned ObjectBase = 0;
  unsigned FrameWords = 0;
  unsigned VarArgsWord = 0; // Frame word of the first unnamed argument.

public:
  SquallMachineFunctionInfo(const Function &, const TargetSubtargetInfo *) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &Src2DstMBB)
      const override {
    return DestMF.cloneInfo<SquallMachineFunctionInfo>(*this);
  }

  unsigned getObjectBase() const { return ObjectBase; }
  unsigned getFrameWords() const { return FrameWords; }
  unsigned getVarArgsWord() const { return VarArgsWord; }
  void setVarArgsWord(unsigned Word) { VarArgsWord = Word; }
  void setLayout(unsigned Base, unsigned Words) {
    ObjectBase = Base;
    FrameWords = Words;
  }
};

} // namespace llvm

#endif
