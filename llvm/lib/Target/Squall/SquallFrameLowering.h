//===-- SquallFrameLowering.h - Squall activation frames --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALLFRAMELOWERING_H
#define LLVM_LIB_TARGET_SQUALL_SQUALLFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

/// Frames grow upward and are allocated by the caller: the call sequence sets
/// the callee's frame pointer just past the caller's frame. No prologue or
/// epilogue code is emitted.
class SquallFrameLowering : public TargetFrameLowering {
public:
  /// Frames are a multiple of 16 logical bytes (four words), so objects
  /// aligned up to 16 bytes keep their alignment in every activation.
  static constexpr unsigned StackAlignBytes = 16;

  SquallFrameLowering()
      : TargetFrameLowering(StackGrowsUp, Align(StackAlignBytes), 0) {}

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &, MachineBasicBlock &) const override {}

protected:
  bool hasFPImpl(const MachineFunction &) const override { return false; }
};

} // namespace llvm

#endif
