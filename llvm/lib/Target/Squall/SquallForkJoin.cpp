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
// frame. Memory accesses may be in both lanes when they cannot alias, but
// those through the dynamic memory helper stay in one lane, because its
// mailboxes are shared. Calls are boundaries unless the callee cannot call
// back into this function; such a call may run in one lane, which joins on
// a word of its own.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallInstrInfo.h"
#include "SquallRegisterInfo.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include <numeric>

using namespace llvm;

#define DEBUG_TYPE "squall-lanes"

static cl::opt<bool>
    ForkJoin("squall-fork-join", cl::init(false),
             cl::desc("Run independent instruction groups within a basic "
                      "block as two parallel tokens"));
static cl::opt<unsigned>
    MinLane("squall-fork-join-min-lane", cl::init(3), cl::Hidden,
            cl::desc("Minimum weight of each lane for a fork: one per "
                     "instruction, eight per call"));

namespace {

class SquallForkJoin : public MachineFunctionPass {
public:
  static char ID;
  SquallForkJoin() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Squall fork/join"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AAResultsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  AAResults *AA = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  SmallVector<MCPhysReg, 128> Unused; // Registers the function never touches.
  const Function *Caller = nullptr;
  unsigned NextID = 0;
  unsigned NextCallJoin = 0; // Reserved call join words handed out.
  DenseMap<std::pair<const Function *, const Function *>, bool> Reaches;

  /// Whether From may call To, directly or through other functions; an
  /// indirect call may reach anything.
  bool mayReach(const Function *From, const Function *To);
  /// A direct call to a function that cannot reach the caller again. A lane
  /// may contain such calls: the region cannot be re-entered while its join
  /// is pending.
  bool isLaneCall(const MachineInstr &MI);
  bool isBoundary(const MachineInstr &MI);
  bool forkRegion(MachineBasicBlock &MBB, MachineBasicBlock::iterator Begin,
                  MachineBasicBlock::iterator End);
};

} // namespace

char SquallForkJoin::ID = 0;

INITIALIZE_PASS_BEGIN(SquallForkJoin, DEBUG_TYPE, "Squall fork/join", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(SquallForkJoin, DEBUG_TYPE, "Squall fork/join", false,
                    false)

FunctionPass *llvm::createSquallForkJoinPass() { return new SquallForkJoin(); }

bool llvm::Squall::isForkJoinEnabled() { return ForkJoin; }

bool SquallForkJoin::mayReach(const Function *From, const Function *To) {
  auto [It, New] = Reaches.try_emplace({From, To}, true);
  if (!New)
    return It->second; // Also true while From is being visited: a cycle.
  bool Result = false;
  SmallPtrSet<const Function *, 16> Seen{From};
  SmallVector<const Function *, 16> Work{From};
  while (!Work.empty() && !Result) {
    const Function *F = Work.pop_back_val();
    for (const Instruction &I : instructions(*F)) {
      const auto *Call = dyn_cast<CallBase>(&I);
      if (!Call)
        continue;
      const Function *Callee = Call->getCalledFunction();
      if (!Callee || Callee == To) {
        Result = true;
        break;
      }
      if (!Callee->isDeclaration() && Seen.insert(Callee).second)
        Work.push_back(Callee);
    }
  }
  Reaches[{From, To}] = Result;
  return Result;
}

bool SquallForkJoin::isLaneCall(const MachineInstr &MI) {
  if (MI.getOpcode() != Squall::CALL && MI.getOpcode() != Squall::CALLR)
    return false;
  const MachineOperand &Target = MI.getOperand(MI.getOpcode() == Squall::CALLR);
  const auto *Callee =
      Target.isGlobal() ? dyn_cast<Function>(Target.getGlobal()) : nullptr;
  return Callee && !Callee->isDeclaration() && !mayReach(Callee, Caller);
}

bool SquallForkJoin::isBoundary(const MachineInstr &MI) {
  if (MI.isCall())
    return !isLaneCall(MI);
  return MI.isTerminator() || MI.isDebugOrPseudoInstr() ||
         MI.isMetaInstruction() || MI.hasUnmodeledSideEffects();
}

bool SquallForkJoin::runOnMachineFunction(MachineFunction &MF) {
  if (!ForkJoin)
    return false;
  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  Caller = &MF.getFunction();
  TRI = MF.getSubtarget().getRegisterInfo();
  TII = MF.getSubtarget().getInstrInfo();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  Unused.clear();
  for (MCPhysReg Reg : Squall::GPRRegClass)
    if (!MRI.isPhysRegUsed(Reg) && !MRI.isReserved(Reg))
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
      // Long blocks are scheduled in pieces; the search is quadratic.
      for (unsigned Size = 0; End != E && !isBoundary(*End) && Size < 128;
           ++Size)
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
  // A call runs a whole function: it weighs as several instructions.
  auto Weight = [](const MachineInstr &MI) { return MI.isCall() ? 8u : 1u; };
  SmallVector<MachineInstr *, 32> MIs;
  unsigned Total = 0;
  for (auto I = Begin; I != End; ++I) {
    MIs.push_back(&*I);
    Total += Weight(*I);
  }
  if (Total < 2 * MinLane)
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

  // Instructions conflict when they share a register that either defines, or
  // when both access memory and one stores to what the other may access.
  // Accesses through computed pointers, and to globals that may be past the
  // static slot range, use the memory helper's shared mailboxes, so at most
  // one lane makes them; frame objects are static slots. Conflicting
  // instructions form a group, which runs in one lane in program order.
  auto UsesHelper = [](const MachineInstr &MI) {
    // A callee may access any memory, and runs in the callee frame that every
    // call site shares.
    if (MI.isCall())
      return true;
    switch (MI.getOpcode()) {
    case Squall::LDP:
    case Squall::STP:
    case Squall::LDG:
    case Squall::STG:
      return true;
    default:
      return false;
    }
  };
  auto Conflict = [&](const MachineInstr &A, const MachineInstr &B) {
    if (A.mayLoadOrStore() && B.mayLoadOrStore() &&
        ((UsesHelper(A) && UsesHelper(B)) ||
         ((A.mayStore() || B.mayStore()) &&
          A.mayAlias(AA, B, /*UseTBAA=*/false))))
      return true;
    for (const MachineOperand &X : A.operands())
      for (const MachineOperand &Y : B.operands())
        if (X.isReg() && Y.isReg() && X.getReg() && X.getReg() == Y.getReg() &&
            (X.isDef() || Y.isDef()))
          return true;
    return false;
  };
  unsigned N = MIs.size();
  SmallVector<BitVector, 32> Earlier(N, BitVector(N)); // Conflicts with I < J.
  for (unsigned I = 0; I < N; ++I)
    for (unsigned J = 0; J < I; ++J)
      if (Conflict(*MIs[J], *MIs[I]))
        Earlier[I].set(J);

  // A plan runs a set of instructions after the join and splits the rest
  // into two lanes. The set is closed under later conflicts: an instruction
  // that conflicts with one after the join must follow it there.
  struct Plan {
    BitVector After;
    SmallVector<unsigned, 32> Group;
    DenseMap<unsigned, bool> InLaneB;
    unsigned Smaller = 0; // Weight of the smaller lane.
  };
  auto MakePlan = [&](BitVector After) {
    Plan P;
    for (unsigned I = 0; I < N; ++I)
      if (!After[I] && Earlier[I].anyCommon(After))
        After.set(I);
    P.Group.resize(N);
    std::iota(P.Group.begin(), P.Group.end(), 0u);
    auto Find = [&](unsigned I) {
      while (P.Group[I] != I)
        I = P.Group[I] = P.Group[P.Group[I]];
      return I;
    };
    for (unsigned I = 0; I < N; ++I)
      if (!After[I])
        for (unsigned J : Earlier[I].set_bits())
          if (unsigned A = Find(I), B = Find(J); A != B)
            P.Group[A] = B;
    for (unsigned I = 0; I < N; ++I)
      P.Group[I] = Find(I);
    // Balance whole groups between the lanes, largest first.
    DenseMap<unsigned, unsigned> Sizes;
    for (unsigned I = 0; I < N; ++I)
      if (!After[I])
        Sizes[P.Group[I]] += Weight(*MIs[I]);
    SmallVector<std::pair<unsigned, unsigned>, 8> Groups(Sizes.begin(),
                                                         Sizes.end());
    llvm::sort(Groups, [](const auto &A, const auto &B) {
      return A.second != B.second ? A.second > B.second : A.first < B.first;
    });
    unsigned LaneA = 0, LaneB = 0;
    for (const auto &[Root, Size] : Groups) {
      bool B = LaneB < LaneA;
      P.InLaneB[Root] = B;
      (B ? LaneB : LaneA) += Size;
    }
    P.Smaller = std::min(LaneA, LaneB);
    P.After = std::move(After);
    return P;
  };

  // The merge points: instructions that combine two groups when everything
  // runs in lanes. Deferring the last J of them to after the join, for each
  // J, finds the cut that leaves the most balanced lanes; deferring a merge
  // that combines small address computations would pull a whole chain out.
  SmallVector<unsigned, 8> Merges;
  {
    SmallVector<unsigned, 32> Group(N);
    std::iota(Group.begin(), Group.end(), 0u);
    auto Find = [&](unsigned I) {
      while (Group[I] != I)
        I = Group[I] = Group[Group[I]];
      return I;
    };
    for (unsigned I = 0; I < N; ++I) {
      SmallSetVector<unsigned, 4> Roots;
      for (unsigned J : Earlier[I].set_bits())
        Roots.insert(Find(J));
      if (Roots.size() > 1)
        Merges.push_back(I);
      for (unsigned R : Roots)
        Group[R] = I;
    }
  }
  Plan Best = MakePlan(BitVector(N));
  BitVector After(N);
  for (unsigned M : reverse(Merges)) {
    After.set(M);
    Plan P = MakePlan(After);
    if (P.Smaller > Best.Smaller)
      Best = std::move(P);
  }
  if (Best.Smaller < MinLane) {
    for (auto &[MO, Reg] : reverse(Renamed))
      MO->setReg(Reg);
    return false;
  }

  // A lane that calls joins on a reserved word of its own: the callee may
  // fork and join on the shared word while this join is pending.
  unsigned Slot = 0;
  for (unsigned I = 0; I < N && !Slot; ++I)
    if (!Best.After[I] && MIs[I]->isCall()) {
      if (NextCallJoin == Squall::NumCallJoinWords) {
        for (auto &[MO, Reg] : reverse(Renamed))
          MO->setReg(Reg);
        return false;
      }
      Slot = 1 + NextCallJoin++;
    }

  // FORK; lane A; LANE; lane B; JOIN; the rest, each in program order.
  DebugLoc DL = MIs.front()->getDebugLoc();
  int64_t ID = NextID++;
  BuildMI(MBB, Begin, DL, TII->get(Squall::FORK)).addImm(ID);
  MachineInstr *Lane =
      BuildMI(MBB, Begin, DL, TII->get(Squall::LANE)).addImm(ID);
  MachineInstr *Join =
      BuildMI(MBB, Begin, DL, TII->get(Squall::JOIN)).addImm(ID).addImm(Slot);
  for (unsigned I = 0; I < N; ++I) {
    MBB.splice(Best.After[I]                 ? End
               : Best.InLaneB[Best.Group[I]] ? Join->getIterator()
                                             : Lane->getIterator(),
               &MBB, MIs[I]->getIterator());
    // Both lanes may read a register; a kill in one no longer ends it.
    MIs[I]->clearKillInfo();
  }
  return true;
}
