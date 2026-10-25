//===-- SquallCheckIR.cpp - Reject IR outside the Squall ABI ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall memory consists of 64-bit matching slots. The backend maps each
// four-byte little-endian cell to one slot, so a byte address is four times a
// word address plus the byte's position in the cell. Floating point, atomics,
// and code outside the module cannot be represented. This pass reports such
// constructs with source locations before instruction selection, instead of
// letting legalization fail later. Failing modules have their function bodies
// replaced and are marked, so code generation completes quietly after the
// diagnostics.
//
//===----------------------------------------------------------------------===//

#include "Squall.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "squall-check-ir"

namespace {

class SquallCheckIR : public ModulePass {
  bool Failed = false;

  void fail(const Function &F, const Twine &Message, const DebugLoc &DL = {}) {
    F.getContext().diagnose(DiagnosticInfoUnsupported(F, Message, DL));
    Failed = true;
  }
  void fail(const GlobalVariable &G, const Twine &Message) {
    G.getContext().diagnose(DiagnosticInfoGeneric(
        Twine("Squall: global '") + G.getName() + "': " + Message));
    Failed = true;
  }

  static bool isValueType(const Type *T) {
    if (T->isPointerTy())
      return T->getPointerAddressSpace() == 0;
    return T->isIntegerTy() && T->getIntegerBitWidth() <= 64;
  }
  /// Integers, pointers, and arrays and structs of them.
  static bool isMemoryType(const Type *T) {
    if (isValueType(T))
      return true;
    if (const auto *A = dyn_cast<ArrayType>(T))
      return isMemoryType(A->getElementType());
    if (const auto *S = dyn_cast<StructType>(T))
      return !S->isOpaque() && all_of(S->elements(), isMemoryType);
    return false;
  }
  static bool isSupportedInitializer(const Constant *C, const DataLayout &DL) {
    if (isa<ConstantInt>(C) || isa<ConstantPointerNull>(C) ||
        isa<ConstantAggregateZero>(C) || isa<UndefValue>(C) ||
        isa<GlobalVariable>(C))
      return true;
    if (const auto *F = dyn_cast<Function>(C))
      return !F->isDeclaration();
    // The address of a global variable plus a constant offset.
    GlobalValue *GV;
    APInt Offset;
    if (isa<ConstantExpr>(C) &&
        IsConstantOffsetFromGlobal(const_cast<Constant *>(C), GV, Offset, DL))
      return isa<GlobalVariable>(GV) && C->getType()->isPointerTy();
    if (isa<ConstantDataSequential>(C) || isa<ConstantAggregate>(C)) {
      unsigned N = isa<ConstantDataSequential>(C)
                       ? cast<ConstantDataSequential>(C)->getNumElements()
                       : C->getNumOperands();
      for (unsigned I = 0; I != N; ++I)
        if (!isSupportedInitializer(C->getAggregateElement(I), DL))
          return false;
      return true;
    }
    return false;
  }

  void checkMemoryAccess(const Function &F, const Instruction &I, Type *T,
                         bool Atomic) {
    if (!isMemoryType(T))
      fail(F, "Squall memory accesses must be integers or pointers",
           I.getDebugLoc());
    // Volatile accesses need no special treatment: expanded code runs with a
    // single control token, in program order.
    if (Atomic)
      fail(F, "atomic accesses are not supported on Squall", I.getDebugLoc());
  }

  void checkFunction(const Function &F) {
    if (F.isVarArg())
      fail(F, "variadic functions are not supported on Squall");
    if (F.arg_size() > Squall::NumArgRegs)
      fail(F, "Squall functions take at most 16 arguments");
    for (const Argument &A : F.args())
      if (!isValueType(A.getType()) || A.hasInAllocaAttr() ||
          A.hasPreallocatedAttr() ||
          (A.hasByValAttr() && !isMemoryType(A.getParamByValType())))
        fail(F, "Squall arguments must be integers of at most 64 bits, "
                "pointers, or byval aggregates of them");
    if (!F.getReturnType()->isVoidTy() && !isValueType(F.getReturnType()))
      fail(F, "Squall return values must be integers of at most 64 bits or "
              "pointers");
    for (const Instruction &I : instructions(F)) {
      const DebugLoc &Loc = I.getDebugLoc();
      if (!I.getType()->isVoidTy() && !isValueType(I.getType()))
        fail(F,
             Twine("unsupported value type in '") + I.getOpcodeName() +
                 "'; Squall supports integers of at most 64 bits and "
                 "pointers",
             Loc);
      if (const auto *L = dyn_cast<LoadInst>(&I)) {
        checkMemoryAccess(F, I, L->getType(), L->isAtomic());
      } else if (const auto *S = dyn_cast<StoreInst>(&I)) {
        checkMemoryAccess(F, I, S->getValueOperand()->getType(), S->isAtomic());
      } else if (const auto *A = dyn_cast<AllocaInst>(&I)) {
        if (!A->isStaticAlloca())
          fail(F,
               "dynamic or non-entry-block allocas are not supported on "
               "Squall",
               Loc);
        if (A->getAlign() > Align(16))
          fail(F,
               "Squall frame objects support alignment of at most 16 "
               "bytes",
               Loc);
        if (A->getAddressSpace())
          fail(F, "nondefault address spaces are not supported on Squall", Loc);
      } else if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
                 isa<FenceInst>(I)) {
        fail(F, "atomic operations are not supported on Squall", Loc);
      } else if (isa<InvokeInst>(I) || isa<LandingPadInst>(I) ||
                 isa<ResumeInst>(I) || isa<CallBrInst>(I) ||
                 isa<IndirectBrInst>(I)) {
        fail(F, Twine("'") + I.getOpcodeName() + "' is not supported on Squall",
             Loc);
      } else if (const auto *Call = dyn_cast<CallInst>(&I)) {
        if (Call->isInlineAsm()) {
          fail(F, "inline assembly is not supported on Squall", Loc);
          continue;
        }
        const Function *Callee = Call->getCalledFunction();
        if (Callee && !Callee->isIntrinsic() && Callee->isDeclaration())
          fail(F,
               Twine("call to '") + Callee->getName() +
                   "' has no definition in this module; Squall has no "
                   "linker or runtime library",
               Loc);
      }
      // A function pointer is its target's entry word, so the target must be
      // in this module.
      for (const Use &Op : I.operands())
        if (const auto *Target = dyn_cast<Function>(Op.get());
            Target && Target->isDeclaration() && !Target->isIntrinsic() &&
            !(isa<CallInst>(I) &&
              &Op == &cast<CallInst>(I).getCalledOperandUse()))
          fail(F,
               Twine("'") + Target->getName() +
                   "' has no definition in this module; Squall has no "
                   "linker or runtime library",
               Loc);
    }
  }

  void checkGlobal(const GlobalVariable &G) {
    const DataLayout &DL = G.getDataLayout();
    if (G.getName().starts_with("llvm.")) {
      if (G.getName() != "llvm.used" && G.getName() != "llvm.compiler.used")
        fail(G, "static constructors and special globals are not supported");
      return;
    }
    if (!G.hasInitializer())
      fail(G, "external globals have no definition; Squall has no linker");
    else if (!isSupportedInitializer(G.getInitializer(), DL))
      fail(G, "initializers may contain only integers, null, addresses "
              "within global variables, and functions defined in this module");
    if (G.isThreadLocal() || G.getAddressSpace())
      fail(G, "thread-local and nondefault address space globals are not "
              "supported");
    if (!isMemoryType(G.getValueType()))
      fail(G, "global storage must consist of integers and pointers");
    if (G.getAlign().valueOrOne() > Align(4096))
      fail(G, "global alignment must be at most 4096 bytes");
  }

public:
  static char ID;
  SquallCheckIR() : ModulePass(ID) {}

  StringRef getPassName() const override { return "Squall IR validation"; }

  bool runOnModule(Module &M) override {
    for (const GlobalVariable &G : M.globals())
      checkGlobal(G);
    for (const Function &F : M)
      if (!F.isDeclaration())
        checkFunction(F);
    if (!Failed)
      return false;
    // The errors are reported; leave nothing for code generation to trip on.
    // Bodies become unreachable rather than being deleted, because the image
    // header, already emitted, refers to the functions.
    for (Function &F : M)
      if (!F.isDeclaration()) {
        F.dropAllReferences();
        new UnreachableInst(M.getContext(),
                            BasicBlock::Create(M.getContext(), "", &F));
      }
    M.getOrInsertNamedMetadata("squall.invalid");
    return true;
  }
};

} // namespace

char SquallCheckIR::ID = 0;

INITIALIZE_PASS(SquallCheckIR, DEBUG_TYPE, "Squall IR validation", false, true)

ModulePass *llvm::createSquallCheckIRPass() { return new SquallCheckIR(); }
