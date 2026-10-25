//===-- SquallForkJoin.cpp - Run independent instructions in parallel ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Expanded Squall code normally runs with one control token. This pass finds,
// within a basic block, instructions between calls and terminators that form
// independent groups (no shared register, at most one group touching memory)
// and arranges them as two lanes that run as separate tokens, followed by the
// instructions that combine their results:
//
//   FORK id; <lane A>; LANE id; <lane B>; JOIN id; <after the join>
//
// The AsmPrinter expands the markers into a fork word, whose secondary token
// starts lane A while the primary jumps to lane B, and a join word that waits
// for both tokens on an empty slot.
//
// It runs after register allocation, when register names are final, and
// before prologue insertion, so registers it adds are part of the frame.
// Register allocation reuses registers for unrelated values, which makes
// independent computations look dependent; temporaries whose whole live range
// lies inside a region are first renamed to registers the function does not
// use. Calls are region boundaries because every call site shares the callee
// frame, and memory accesses stay in one lane because the dynamic memory
// helper's mailboxes are shared.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallInstrInfo.h"
#include "SquallRegisterInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/CommandLine.h"
#include <numeric>

using namespace llvm;

#define DEBUG_TYPE "squall-fork-join"

static cl::opt<bool>
    ForkJoin("squall-fork-join", cl::init(false),
             cl::desc("Run independent instruction groups within a basic "
                      "block as two parallel tokens"));
static cl::opt<unsigned>
    MinLane("squall-fork-join-min-lane", cl::init(3), cl::Hidden,
            cl::desc("Minimum instructions in each lane for a fork"));

namespace {

class SquallForkJoin : public MachineFunctionPass {
public:
  static char ID;
  SquallForkJoin() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Squall fork/join"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  SmallVector<MCPhysReg, 128> Unused; // Registers the function never touches.
  unsigned NextID = 0;

  bool forkRegion(MachineBasicBlock &MBB, MachineBasicBlock::iterator Begin,
                  MachineBasicBlock::iterator End);
};

} // namespace

char SquallForkJoin::ID = 0;

INITIALIZE_PASS(SquallForkJoin, DEBUG_TYPE, "Squall fork/join", false, false)

FunctionPass *llvm::createSquallForkJoinPass() { return new SquallForkJoin(); }

static bool isBoundary(const MachineInstr &MI) {
  return MI.isCall() || MI.isTerminator() || MI.isDebugOrPseudoInstr() ||
         MI.isMetaInstruction();
}

bool SquallForkJoin::runOnMachineFunction(MachineFunction &MF) {
  if (!ForkJoin)
    return false;
  TRI = MF.getSubtarget().getRegisterInfo();
  TII = MF.getSubtarget().getInstrInfo();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  Unused.clear();
  for (MCPhysReg Reg : Squall::GPRRegClass)
    if (!MRI.isPhysRegUsed(Reg))
      Unused.push_back(Reg);

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    auto I = MBB.begin(), E = MBB.end();
    while (I != E) {
      if (isBoundary(*I)) {
        ++I;
        continue;
      }
      auto End = I;
      while (End != E && !isBoundary(*End))
        ++End;
      Changed |= forkRegion(MBB, I, End);
      I = End;
    }
  }
  return Changed;
}

bool SquallForkJoin::forkRegion(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator Begin,
                                MachineBasicBlock::iterator End) {
  SmallVector<MachineInstr *, 32> MIs;
  for (auto I = Begin; I != End; ++I)
    MIs.push_back(&*I);
  if (MIs.size() < 2 * MinLane)
    return false;

  // Registers live after the region.
  LiveRegUnits LiveAfter(*TRI);
  LiveAfter.addLiveOuts(MBB);
  for (auto I = MBB.end(); I != End;)
    LiveAfter.stepBackward(*--I);

  // Rename each value defined and entirely consumed inside the region to an
  // unused register, so only true dependencies remain. Renames are undone
  // if the region does not fork.
  SmallVector<std::pair<MachineOperand *, Register>, 32> Renamed;
  DenseMap<Register, Register> Current; // Original name -> live name.
  unsigned Fresh = 0;
  for (unsigned I = 0; I < MIs.size(); ++I) {
    for (MachineOperand &MO : MIs[I]->operands())
      if (MO.isReg() && MO.isUse() && Current.count(MO.getReg())) {
        Renamed.push_back({&MO, MO.getReg()});
        MO.setReg(Current[MO.getReg()]);
      }
    for (MachineOperand &MO : MIs[I]->operands()) {
      if (!MO.isReg() || !MO.isDef() || !MO.getReg().isPhysical())
        continue;
      Register Reg = MO.getReg();
      Current.erase(Reg);
      // The value is local unless it reaches the end of the region live.
      bool Redefined = false;
      for (unsigned J = I + 1; J < MIs.size() && !Redefined; ++J)
        Redefined = MIs[J]->definesRegister(Reg, TRI);
      if ((!Redefined && !LiveAfter.available(Reg)) || Fresh == Unused.size())
        continue;
      Register New = Unused[Fresh++];
      Renamed.push_back({&MO, Reg});
      MO.setReg(New);
      Current[Reg] = New;
    }
  }

  // Each instruction joins the group of the instructions it conflicts with:
  // a shared register that either defines, or both accessing memory. One that
  // would merge two groups combines the lanes' results, so it runs after the
  // join, as does everything that conflicts with such an instruction.
  auto Conflict = [](const MachineInstr &A, const MachineInstr &B) {
    if (A.mayLoadOrStore() && B.mayLoadOrStore())
      return true;
    for (const MachineOperand &X : A.operands())
      for (const MachineOperand &Y : B.operands())
        if (X.isReg() && Y.isReg() && X.getReg() && X.getReg() == Y.getReg() &&
            (X.isDef() || Y.isDef()))
          return true;
    return false;
  };
  SmallVector<unsigned, 32> Group(MIs.size());
  std::iota(Group.begin(), Group.end(), 0u);
  auto Find = [&](unsigned I) {
    while (Group[I] != I)
      I = Group[I] = Group[Group[I]];
    return I;
  };
  SmallVector<bool, 32> AfterJoin(MIs.size());
  for (unsigned I = 0; I < MIs.size(); ++I) {
    SmallSetVector<unsigned, 4> Roots;
    for (unsigned J = 0; J < I && !AfterJoin[I]; ++J) {
      if (!Conflict(*MIs[J], *MIs[I]))
        continue;
      if (AfterJoin[J])
        AfterJoin[I] = true;
      else
        Roots.insert(Find(J));
    }
    if (Roots.size() > 1)
      AfterJoin[I] = true;
    else if (!AfterJoin[I] && Roots.size() == 1)
      Group[I] = Roots.front();
  }

  // Balance whole groups between the lanes, largest first.
  DenseMap<unsigned, unsigned> Sizes;
  for (unsigned I = 0; I < MIs.size(); ++I)
    if (!AfterJoin[I])
      ++Sizes[Find(I)];
  SmallVector<std::pair<unsigned, unsigned>, 8> Groups(Sizes.begin(),
                                                       Sizes.end());
  llvm::sort(Groups, [](const auto &A, const auto &B) {
    return A.second != B.second ? A.second > B.second : A.first < B.first;
  });
  DenseMap<unsigned, bool> InLaneB;
  unsigned LaneA = 0, LaneB = 0;
  for (const auto &[Root, Size] : Groups) {
    bool B = LaneB < LaneA;
    InLaneB[Root] = B;
    (B ? LaneB : LaneA) += Size;
  }
  if (std::min(LaneA, LaneB) < MinLane) {
    for (auto &[MO, Reg] : reverse(Renamed))
      MO->setReg(Reg);
    return false;
  }

  // FORK; lane A; LANE; lane B; JOIN; the rest, each in program order.
  DebugLoc DL = MIs.front()->getDebugLoc();
  int64_t ID = NextID++;
  BuildMI(MBB, Begin, DL, TII->get(Squall::FORK)).addImm(ID);
  MachineInstr *Lane =
      BuildMI(MBB, Begin, DL, TII->get(Squall::LANE)).addImm(ID);
  MachineInstr *Join =
      BuildMI(MBB, Begin, DL, TII->get(Squall::JOIN)).addImm(ID);
  for (unsigned I = 0; I < MIs.size(); ++I) {
    MBB.splice(AfterJoin[I]       ? End
               : InLaneB[Find(I)] ? Join->getIterator()
                                  : Lane->getIterator(),
               &MBB, MIs[I]->getIterator());
    // Both lanes may read a register; a kill in one no longer ends it.
    MIs[I]->clearKillInfo();
  }
  return true;
}
