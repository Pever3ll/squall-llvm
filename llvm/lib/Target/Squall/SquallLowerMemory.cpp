//===-- SquallLowerMemory.cpp - Rewrite partial-cell stores -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall memory is a sequence of four-byte cells, each one 64-bit slot. A
// store that covers less than a whole aligned cell must load the cell, merge
// the new bytes, and store it back. This pass performs that expansion in IR
// rather than during instruction selection: SelectionDAG may place stores to
// different bytes of one cell on independent chains because they do not
// alias, and two read-modify-writes of the same cell must not interleave.
// Rewritten as i32 accesses to the same address, the updates alias and stay
// in program order. Narrow loads are left to instruction selection.
//
// Memory intrinsics that are not whole aligned cells become loops first, and
// stack objects and globals are aligned to cells, which the layout already
// guarantees, so offsets from them have statically known byte positions.
//
//===----------------------------------------------------------------------===//

#include "Squall.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"

using namespace llvm;

#define DEBUG_TYPE "squall-lower-memory"

namespace {

class SquallLowerMemory : public ModulePass {
public:
  static char ID;
  SquallLowerMemory() : ModulePass(ID) {}

  StringRef getPassName() const override { return "Squall memory lowering"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }
  bool runOnModule(Module &M) override;

private:
  const DataLayout *DL = nullptr;

  bool lowerFunction(Function &F);
  void splitAggregateStore(StoreInst *S);
  void lowerStore(StoreInst *S);
  void updateCell(IRBuilder<> &B, Value *Ptr, Value *Field, unsigned Bytes,
                  bool Volatile);
};

} // namespace

char SquallLowerMemory::ID = 0;

INITIALIZE_PASS_BEGIN(SquallLowerMemory, DEBUG_TYPE, "Squall memory lowering",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(SquallLowerMemory, DEBUG_TYPE, "Squall memory lowering",
                    false, false)

ModulePass *llvm::createSquallLowerMemoryPass() {
  return new SquallLowerMemory();
}

static bool isWholeCells(const MemIntrinsic &MI) {
  auto *Length = dyn_cast<ConstantInt>(MI.getLength());
  bool Aligned = MI.getDestAlign().valueOrOne() >= Align(4);
  if (const auto *Transfer = dyn_cast<MemTransferInst>(&MI))
    Aligned &= Transfer->getSourceAlign().valueOrOne() >= Align(4);
  return Aligned && Length && Length->getZExtValue() % 4 == 0;
}

static bool coversWholeCells(const StoreInst &S, const DataLayout &DL) {
  return DL.getTypeStoreSize(S.getValueOperand()->getType()) % 4 == 0 &&
         S.getAlign() >= Align(4);
}

bool SquallLowerMemory::runOnModule(Module &M) {
  // IR validation reported errors and emptied the module.
  if (M.getNamedMetadata("squall.invalid"))
    return false;
  DL = &M.getDataLayout();
  bool Changed = false;
  for (GlobalVariable &G : M.globals())
    if (G.hasInitializer() && G.getAlign().valueOrOne() < Align(4)) {
      G.setAlignment(Align(4));
      Changed = true;
    }
  for (Function &F : M)
    if (!F.isDeclaration())
      Changed |= lowerFunction(F);
  return Changed;
}

bool SquallLowerMemory::lowerFunction(Function &F) {
  const TargetTransformInfo &TTI =
      getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  SmallVector<MemIntrinsic *, 8> Intrinsics;
  bool Changed = false;
  for (Instruction &I : instructions(F)) {
    if (auto *A = dyn_cast<AllocaInst>(&I); A && A->getAlign() < Align(4)) {
      A->setAlignment(Align(4));
      Changed = true;
    } else if (auto *MI = dyn_cast<MemIntrinsic>(&I);
               MI && !isWholeCells(*MI)) {
      Intrinsics.push_back(MI);
    }
  }
  for (MemIntrinsic *MI : Intrinsics) {
    if (auto *Copy = dyn_cast<MemCpyInst>(MI))
      expandMemCpyAsLoop(Copy, TTI);
    else if (auto *Move = dyn_cast<MemMoveInst>(MI))
      expandMemMoveAsLoop(Move, TTI);
    else
      expandMemSetAsLoop(cast<MemSetInst>(MI), TTI);
    MI->eraseFromParent();
    Changed = true;
  }

  SmallVector<StoreInst *, 16> Stores;
  for (Instruction &I : instructions(F))
    if (auto *S = dyn_cast<StoreInst>(&I);
        S && S->getValueOperand()->getType()->isAggregateType())
      Stores.push_back(S);
  for (StoreInst *S : Stores)
    splitAggregateStore(S);
  Changed |= !Stores.empty();

  Stores.clear();
  for (Instruction &I : instructions(F))
    if (auto *S = dyn_cast<StoreInst>(&I); S && !coversWholeCells(*S, *DL))
      Stores.push_back(S);
  for (StoreInst *S : Stores)
    lowerStore(S);
  return Changed || !Stores.empty();
}

// A first-class aggregate store becomes one store per element, in order.
void SquallLowerMemory::splitAggregateStore(StoreInst *S) {
  SmallVector<StoreInst *, 8> Work{S};
  while (!Work.empty()) {
    StoreInst *Store = Work.pop_back_val();
    Value *V = Store->getValueOperand();
    auto *T = V->getType();
    if (!T->isAggregateType())
      continue;
    IRBuilder<> B(Store);
    unsigned N = isa<StructType>(T) ? cast<StructType>(T)->getNumElements()
                                    : cast<ArrayType>(T)->getNumElements();
    const StructLayout *L =
        isa<StructType>(T) ? DL->getStructLayout(cast<StructType>(T)) : nullptr;
    for (unsigned I = 0; I < N; ++I) {
      Type *E = isa<StructType>(T) ? cast<StructType>(T)->getElementType(I)
                                   : cast<ArrayType>(T)->getElementType();
      uint64_t Offset =
          L ? L->getElementOffset(I) : I * DL->getTypeAllocSize(E);
      Value *Ptr =
          B.CreatePtrAdd(Store->getPointerOperand(), B.getInt32(Offset));
      StoreInst *Element = B.CreateAlignedStore(
          B.CreateExtractValue(V, I), Ptr,
          commonAlignment(Store->getAlign(), Offset), Store->isVolatile());
      Work.push_back(Element);
    }
    Store->eraseFromParent();
  }
}

void SquallLowerMemory::lowerStore(StoreInst *S) {
  IRBuilder<> B(S);
  Value *V = S->getValueOperand();
  unsigned Bytes = DL->getTypeStoreSize(V->getType());
  if (V->getType()->isPointerTy())
    V = B.CreatePtrToInt(V, B.getIntNTy(Bytes * 8));
  else
    V = B.CreateZExt(V, B.getIntNTy(Bytes * 8));
  // Pieces that never straddle a cell boundary.
  unsigned Piece = Bytes % 2 == 0 && S->getAlign() >= Align(2) ? 2 : 1;
  if (Bytes == 1 || (Bytes == 2 && Piece == 2)) {
    updateCell(B, S->getPointerOperand(), V, Bytes, S->isVolatile());
  } else {
    for (unsigned Offset = 0; Offset < Bytes; Offset += Piece) {
      Value *Field =
          B.CreateTrunc(B.CreateLShr(V, Offset * 8), B.getIntNTy(Piece * 8));
      updateCell(B, B.CreatePtrAdd(S->getPointerOperand(), B.getInt32(Offset)),
                 Field, Piece, S->isVolatile());
    }
  }
  S->eraseFromParent();
}

void SquallLowerMemory::updateCell(IRBuilder<> &B, Value *Ptr, Value *Field,
                                   unsigned Bytes, bool Volatile) {
  Type *I32 = B.getInt32Ty();
  APInt Offset(DL->getIndexTypeSizeInBits(Ptr->getType()), 0);
  const Value *Base = Ptr->stripAndAccumulateConstantOffsets(
      *DL, Offset, /*AllowNonInbounds=*/true);
  Value *Cell, *Shift;
  if (isa<AllocaInst>(Base) || isa<GlobalVariable>(Base)) {
    int64_t Position = Offset.getSExtValue();
    Cell = B.CreatePtrAdd(const_cast<Value *>(Base),
                          B.getInt32(Position & ~int64_t(3)));
    Shift = B.getInt32((Position & 3) * 8);
  } else {
    Value *Position = B.CreateAnd(B.CreatePtrToInt(Ptr, I32), 3);
    Cell = B.CreatePtrAdd(Ptr, B.CreateNeg(Position));
    Shift = B.CreateShl(Position, 3);
  }
  Value *Old = B.CreateAlignedLoad(I32, Cell, Align(4), Volatile);
  Value *Hole = B.CreateNot(
      B.CreateShl(B.getInt32(maskTrailingOnes<uint32_t>(Bytes * 8)), Shift));
  // Operands are built in separate statements: argument evaluation order is
  // unspecified, and the IR must not depend on the host compiler.
  Value *Kept = B.CreateAnd(Old, Hole);
  Value *Placed = B.CreateShl(B.CreateZExt(Field, I32), Shift);
  Value *New = B.CreateOr(Kept, Placed);
  B.CreateAlignedStore(New, Cell, Align(4), Volatile);
}
