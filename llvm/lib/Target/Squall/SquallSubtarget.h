//===-- SquallSubtarget.h - Squall subtarget --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALLSUBTARGET_H
#define LLVM_LIB_TARGET_SQUALL_SQUALLSUBTARGET_H

#include "SquallFrameLowering.h"
#include "SquallISelLowering.h"
#include "SquallInstrInfo.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <memory>

#define GET_SUBTARGETINFO_HEADER
#include "SquallGenSubtargetInfo.inc"

namespace llvm {

class SquallSubtarget : public SquallGenSubtargetInfo {
  SquallInstrInfo InstrInfo;
  SquallFrameLowering FrameLowering;
  SquallTargetLowering TLInfo;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;

public:
  SquallSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                  const TargetMachine &TM);
  ~SquallSubtarget() override;

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const SquallInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const SquallRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const TargetFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const SquallTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return TSInfo.get();
  }
};

} // namespace llvm

#endif
