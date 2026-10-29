//===-- SquallRegisterInfo.cpp - Squall register information ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallRegisterInfo.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallFrameLowering.h"
#include "SquallMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "SquallGenRegisterInfo.inc"

SquallRegisterInfo::SquallRegisterInfo() : SquallGenRegisterInfo(Squall::FP) {}

// Every activation has a private frame, so calls preserve all registers and
// there is nothing to save.
const MCPhysReg *
SquallRegisterInfo::getCalleeSavedRegs(const MachineFunction *) const {
  static const MCPhysReg None[] = {0};
  return None;
}

BitVector SquallRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(Squall::FP);
  // va_arg reads unnamed arguments from their registers' frame words at any
  // point in a variadic function, so the allocator must not reuse them.
  if (MF.getFunction().isVarArg())
    for (unsigned I = 0; I < Squall::NumArgRegs; ++I)
      Reserved.set(Squall::GPRRegClass.getRegister(I));
  return Reserved;
}

// Replace a frame object with its frame word. The following operand holds a
// byte offset within the object; frame objects use four-byte logical cells.
bool SquallRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int, unsigned FIOperandNum,
                                             RegScavenger *) const {
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getMF();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const auto *FuncInfo = MF.getInfo<SquallMachineFunctionInfo>();
  MachineOperand &FIOp = MI.getOperand(FIOperandNum);
  MachineOperand &OffsetOp = MI.getOperand(FIOperandNum + 1);
  int64_t Bytes = MFI.getObjectOffset(FIOp.getIndex()) + OffsetOp.getImm();
  if (Bytes < 0 || Bytes % 4)
    report_fatal_error("Squall: frame access is not a four-byte cell", false);
  FIOp.ChangeToImmediate(FuncInfo->getObjectBase() + Bytes / 4);
  OffsetOp.setImm(0);
  return false;
}

Register SquallRegisterInfo::getFrameRegister(const MachineFunction &) const {
  return Squall::FP;
}
