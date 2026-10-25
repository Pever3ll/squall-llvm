//===-- SquallInstrInfo.cpp - Squall instruction information ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallInstrInfo.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "SquallSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "SquallGenInstrInfo.inc"

SquallInstrInfo::SquallInstrInfo(const SquallSubtarget &STI)
    : SquallGenInstrInfo(STI, RI), RI() {}

void SquallInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc, bool,
                                  bool) const {
  BuildMI(MBB, I, DL, get(Squall::MOVrr), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

static MachineMemOperand *frameMemOperand(MachineBasicBlock &MBB, int FI,
                                          MachineMemOperand::Flags Flags) {
  MachineFunction &MF = *MBB.getParent();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI),
                                 Flags, MFI.getObjectSize(FI),
                                 MFI.getObjectAlign(FI));
}

void SquallInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MI,
                                          Register SrcReg, bool IsKill,
                                          int FrameIndex,
                                          const TargetRegisterClass *, Register,
                                          MachineInstr::MIFlag Flags) const {
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  BuildMI(MBB, MI, DL, get(Squall::STF))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FrameIndex)
      .addImm(0)
      .addMemOperand(
          frameMemOperand(MBB, FrameIndex, MachineMemOperand::MOStore))
      .setMIFlags(Flags);
}

void SquallInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIndex,
                                           const TargetRegisterClass *,
                                           Register, unsigned,
                                           MachineInstr::MIFlag Flags) const {
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  BuildMI(MBB, MI, DL, get(Squall::LDF), DestReg)
      .addFrameIndex(FrameIndex)
      .addImm(0)
      .addMemOperand(
          frameMemOperand(MBB, FrameIndex, MachineMemOperand::MOLoad))
      .setMIFlags(Flags);
}

Register SquallInstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                              int &FrameIndex) const {
  if (MI.getOpcode() == Squall::LDF && MI.getOperand(1).isFI() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }
  return Register();
}

Register SquallInstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                             int &FrameIndex) const {
  if (MI.getOpcode() == Squall::STF && MI.getOperand(1).isFI() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }
  return Register();
}

static bool isConditionalBranch(unsigned Opcode) {
  return Opcode == Squall::BRNZ || Opcode == Squall::BRZ;
}

// Branch conditions are [opcode, condition register].
bool SquallInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool) const {
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false;
  if (!I->isBranch())
    return true;
  MachineInstr *Last = &*I;
  MachineInstr *Prior = nullptr;
  if (I != MBB.begin()) {
    MachineBasicBlock::iterator P = std::prev(I);
    while (P != MBB.begin() && P->isDebugInstr())
      --P;
    if (!P->isDebugInstr() && isUnpredicatedTerminator(*P))
      Prior = &*P;
  }
  if (Prior) {
    // Two terminators: a conditional branch followed by an unconditional one.
    if (!isConditionalBranch(Prior->getOpcode()) ||
        Last->getOpcode() != Squall::BR)
      return true;
    MachineBasicBlock::iterator P(Prior);
    if (P != MBB.begin()) {
      MachineBasicBlock::iterator Before = std::prev(P);
      if (!Before->isDebugInstr() && isUnpredicatedTerminator(*Before))
        return true;
    }
    TBB = Prior->getOperand(1).getMBB();
    FBB = Last->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(Prior->getOpcode()));
    Cond.push_back(Prior->getOperand(0));
    return false;
  }
  if (Last->getOpcode() == Squall::BR) {
    TBB = Last->getOperand(0).getMBB();
    return false;
  }
  if (isConditionalBranch(Last->getOpcode())) {
    TBB = Last->getOperand(1).getMBB();
    Cond.push_back(MachineOperand::CreateImm(Last->getOpcode()));
    Cond.push_back(Last->getOperand(0));
    return false;
  }
  return true;
}

unsigned SquallInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  assert(!BytesRemoved && "code size is not counted");
  unsigned Count = 0;
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != Squall::BR && !isConditionalBranch(I->getOpcode()))
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  return Count;
}

unsigned SquallInstrInfo::insertBranch(
    MachineBasicBlock &MBB, MachineBasicBlock *TBB, MachineBasicBlock *FBB,
    ArrayRef<MachineOperand> Cond, const DebugLoc &DL, int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert(!BytesAdded && "code size is not counted");
  if (Cond.empty()) {
    assert(!FBB && "unconditional branch with multiple successors");
    BuildMI(&MBB, DL, get(Squall::BR)).addMBB(TBB);
    return 1;
  }
  BuildMI(&MBB, DL, get(Cond[0].getImm())).add(Cond[1]).addMBB(TBB);
  if (!FBB)
    return 1;
  BuildMI(&MBB, DL, get(Squall::BR)).addMBB(FBB);
  return 2;
}

bool SquallInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  Cond[0].setImm(Cond[0].getImm() == Squall::BRNZ ? Squall::BRZ : Squall::BRNZ);
  return false;
}
