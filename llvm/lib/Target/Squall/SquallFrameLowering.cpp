//===-- SquallFrameLowering.cpp - Squall activation frames --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallFrameLowering.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallMachineFunctionInfo.h"
#include "SquallRegisterInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"

using namespace llvm;

// Frame objects are placed after the highest register the function uses, so
// small functions keep small frames. Prologue insertion runs after register
// allocation and before frame indices are eliminated, which is exactly when
// both pieces of information are final.
void SquallFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  unsigned UsedRegs = 0;
  for (MCPhysReg Reg : Squall::GPRRegClass)
    if (MRI.isPhysRegUsed(Reg))
      UsedRegs = std::max<unsigned>(UsedRegs, TRI->getEncodingValue(Reg) -
                                                  Squall::FirstRegWord + 1);
  // Reserved argument registers of a variadic function hold its arguments.
  if (MF.getFunction().isVarArg())
    UsedRegs = std::max(UsedRegs, Squall::NumArgRegs);
  if (MFI.getMaxAlign() > getStackAlign())
    MF.getFunction().getContext().diagnose(DiagnosticInfoUnsupported(
        MF.getFunction(),
        "Squall frames support object alignment of at most 16 bytes"));
  constexpr unsigned AlignWords = StackAlignBytes / 4;
  unsigned Base = alignTo(Squall::FirstRegWord + UsedRegs, AlignWords);
  uint64_t Words = alignTo(Base + MFI.getStackSize() / 4, AlignWords);
  // The callee's return tag and arguments also live at frame-relative words
  // past this frame, so leave room for them inside the 11-bit slot field.
  if (Words + Squall::FirstRegWord + Squall::NumArgRegs > Squall::MaxSlotWord)
    MF.getFunction().getContext().diagnose(DiagnosticInfoUnsupported(
        MF.getFunction(), "Squall activation frame exceeds 2000 words"));
  MF.getInfo<SquallMachineFunctionInfo>()->setLayout(Base, Words);
}

// Frame slots are four-byte cells, and narrow accesses rely on every object
// starting on a cell boundary.
void SquallFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  for (int FI = MFI.getObjectIndexBegin(); FI != MFI.getObjectIndexEnd(); ++FI)
    if (!MFI.isDeadObjectIndex(FI))
      MFI.setObjectAlignment(FI, std::max(MFI.getObjectAlign(FI), Align(4)));
}
