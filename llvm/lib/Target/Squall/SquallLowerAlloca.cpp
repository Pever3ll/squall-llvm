//===-- SquallLowerAlloca.cpp - Dynamic stack allocation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall activation frames have a fixed size: a callee's frame starts just
// past its caller's. Allocas whose size is not constant, or that are not in
// the entry block (variable-length arrays, alloca()), are allocated instead
// from a separate area that this pass adds to the image, with a bump pointer:
//
//   @__squall_alloca_area = internal global [N x i32] zeroinitializer
//   @__squall_alloca_top  = internal global ptr @__squall_alloca_area
//
// An allocation aligns the top, advances it, and traps when the area is
// exhausted. A function that allocates saves the top on entry and restores
// it before every return, so allocations live exactly as long as their
// activation; llvm.stacksave and llvm.stackrestore read and write the top.
//
//===----------------------------------------------------------------------===//

#include "Squall.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "squall-lower-alloca"

static cl::opt<unsigned> AllocaBytes(
    "squall-alloca-bytes", cl::init(4096),
    cl::desc("Bytes of the Squall area for variable-length arrays and "
             "dynamic allocas"));

namespace {

class SquallLowerAlloca : public ModulePass {
public:
  static char ID;
  SquallLowerAlloca() : ModulePass(ID) {}

  StringRef getPassName() const override {
    return "Squall dynamic stack allocation";
  }
  bool runOnModule(Module &M) override;
};

} // namespace

char SquallLowerAlloca::ID = 0;

INITIALIZE_PASS(SquallLowerAlloca, DEBUG_TYPE,
                "Squall dynamic stack allocation", false, false)

ModulePass *llvm::createSquallLowerAllocaPass() {
  return new SquallLowerAlloca();
}

static bool isStackIntrinsic(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return II && (II->getIntrinsicID() == Intrinsic::stacksave ||
                II->getIntrinsicID() == Intrinsic::stackrestore);
}

bool SquallLowerAlloca::runOnModule(Module &M) {
  SmallVector<Function *, 8> Functions;
  for (Function &F : M)
    if (any_of(instructions(F), [](const Instruction &I) {
          const auto *A = dyn_cast<AllocaInst>(&I);
          return (A && !A->isStaticAlloca()) || isStackIntrinsic(I);
        }))
      Functions.push_back(&F);
  if (Functions.empty())
    return false;

  LLVMContext &C = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  Type *I32 = Type::getInt32Ty(C);
  PointerType *Ptr = PointerType::getUnqual(C);
  auto *AreaType = ArrayType::get(I32, alignTo(AllocaBytes, 16) / 4);
  auto *Area = new GlobalVariable(
      M, AreaType, false, GlobalValue::InternalLinkage,
      ConstantAggregateZero::get(AreaType), "__squall_alloca_area");
  Area->setAlignment(Align(16));
  auto *Top = new GlobalVariable(M, Ptr, false, GlobalValue::InternalLinkage,
                                 Area, "__squall_alloca_top");
  Constant *End = ConstantExpr::getGetElementPtr(
      AreaType, Area,
      ArrayRef<Constant *>{ConstantInt::get(I32, 0),
                           ConstantInt::get(I32, AreaType->getNumElements())});
  Function *Trap = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::trap);

  for (Function *F : Functions) {
    IRBuilder<> Entry(&*F->getEntryBlock().getFirstInsertionPt());
    Value *Saved = Entry.CreateLoad(Ptr, Top, "alloca.saved");

    SmallVector<Instruction *, 16> Work;
    for (Instruction &I : instructions(*F))
      if (isa<ReturnInst>(I) || isStackIntrinsic(I) ||
          (isa<AllocaInst>(I) && !cast<AllocaInst>(I).isStaticAlloca()))
        Work.push_back(&I);

    for (Instruction *I : Work) {
      IRBuilder<> B(I);
      if (isa<ReturnInst>(I)) {
        B.CreateStore(Saved, Top);
        continue;
      }
      if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (II->getIntrinsicID() == Intrinsic::stacksave)
          II->replaceAllUsesWith(B.CreateLoad(Ptr, Top, "stack.top"));
        else
          B.CreateStore(II->getArgOperand(0), Top);
        II->eraseFromParent();
        continue;
      }
      auto *A = cast<AllocaInst>(I);
      // Scalable vectors have no fixed size; SquallCheckIR reports them.
      if (A->getAllocatedType()->isScalableTy())
        continue;
      uint64_t Element = DL.getTypeAllocSize(A->getAllocatedType());
      // No start in the area, which lies below 2^31, is aligned to 2^31 or
      // more. Clamping larger alignments (up to IR's 2^32) to 2^31 keeps the
      // masks in i32, and the aligned start still lands past the area: trap.
      uint64_t Alignment =
          std::clamp<uint64_t>(A->getAlign().value(), 4, UINT64_C(1) << 31);
      Value *Current = B.CreatePtrToInt(B.CreateLoad(Ptr, Top), I32);
      Value *Start = B.CreateAnd(
          B.CreateAdd(Current, ConstantInt::get(I32, Alignment - 1)),
          ConstantInt::getSigned(I32, -int64_t(Alignment)));
      // The request must fit in the room left after aligning the start,
      // which is checked before any arithmetic on the element count can
      // wrap: the count at full width against room / element size. The
      // start and the area's end are whole cells, so a request that fits
      // still fits rounded up to a cell.
      Value *EndWord = B.CreatePtrToInt(End, I32);
      Value *Exhausted = B.CreateICmpUGT(Start, EndWord);
      bool WideElement = Element > UINT32_MAX;
      if (WideElement) {
        // No nonzero count can fit, but a zero count is still valid. Avoid
        // narrowing the element size to i32 (which would assert or wrap).
        Value *Nonzero = B.CreateIsNotNull(A->getArraySize());
        Exhausted = B.CreateOr(Exhausted, Nonzero);
      } else if (Element) {
        Value *Room = B.CreateSub(EndWord, Start);
        Value *Capacity = B.CreateUDiv(Room, ConstantInt::get(I32, Element));
        unsigned Width =
            std::max(64u, A->getArraySize()->getType()->getIntegerBitWidth());
        Type *Wide = IntegerType::get(C, Width);
        Value *Count = B.CreateZExtOrTrunc(A->getArraySize(), Wide);
        Value *TooMany = B.CreateICmpUGT(Count, B.CreateZExt(Capacity, Wide));
        Exhausted = B.CreateOr(Exhausted, TooMany);
      }
      // Size in bytes, rounded up to whole cells; exact when it fits.
      Value *Count = B.CreateZExtOrTrunc(A->getArraySize(), I32);
      Value *Size = WideElement
                        ? B.getInt32(0)
                        : B.CreateMul(Count, ConstantInt::get(I32, Element));
      Size = B.CreateAnd(B.CreateAdd(Size, ConstantInt::get(I32, 3)),
                         ConstantInt::get(I32, ~3u));
      Value *Next = B.CreateAdd(Start, Size);
      Instruction *Fail = SplitBlockAndInsertIfThen(Exhausted, A, true);
      IRBuilder<>(Fail).CreateCall(Trap);
      B.SetInsertPoint(A);
      B.CreateStore(B.CreateIntToPtr(Next, Ptr), Top);
      Value *Pointer = B.CreateIntToPtr(Start, A->getType());
      Pointer->takeName(A);
      A->replaceAllUsesWith(Pointer);
      A->eraseFromParent();
    }
  }
  return true;
}
