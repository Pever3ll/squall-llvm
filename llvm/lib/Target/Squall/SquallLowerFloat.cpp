//===-- SquallLowerFloat.cpp - Software floating point --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall has no floating-point hardware. This pass rewrites float and double
// arithmetic, comparisons, and conversions into calls to the compiler-rt
// soft-float builtins (__addsf3, __ltdf2, __fixsfsi, ...), and the exact
// math intrinsics into bit operations or C library calls. Values of float
// type that are only loaded, stored, selected, or passed are left to
// SelectionDAG, which softens them into integers.
//
// Doing this in IR, before the generic codegen IR passes, means that the
// calls are ordinary IR calls: the IR checker reports missing definitions
// with source locations, and the builtins are compiled like any other
// function. Link-time optimization keeps every runtime library function the
// program defines (they are in llvm.compiler.used, since code generation may
// introduce calls to them); once the calls exist, this pass deletes the ones
// nothing calls, which a linker would otherwise do with dead stripping.
//
//===----------------------------------------------------------------------===//

#include "Squall.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "squall-lower-float"

namespace {

class SquallLowerFloat : public ModulePass {
public:
  static char ID;
  SquallLowerFloat() : ModulePass(ID) {}

  StringRef getPassName() const override {
    return "Squall software floating point";
  }
  bool runOnModule(Module &M) override;

private:
  Module *M = nullptr;

  /// A call to a runtime function, declared if the module lacks it.
  Value *call(IRBuilder<> &B, StringRef Name, Type *Result,
              ArrayRef<Value *> Args);
  bool lower(Instruction &I);
  Value *lowerCompare(IRBuilder<> &B, CmpInst::Predicate P, Value *L, Value *R);
  Value *lowerClassTest(IRBuilder<> &B, Value *V, FPClassTest Mask);
  bool stripUnusedRuntime();
};

} // namespace

char SquallLowerFloat::ID = 0;

INITIALIZE_PASS(SquallLowerFloat, DEBUG_TYPE, "Squall software floating point",
                false, false)

ModulePass *llvm::createSquallLowerFloatPass() {
  return new SquallLowerFloat();
}

static bool isSoftType(const Type *T) {
  return T->isFloatTy() || T->isDoubleTy();
}

/// The builtin suffix for a type: "sf" for float, "df" for double.
static StringRef suffix(const Type *T) { return T->isFloatTy() ? "sf" : "df"; }

static IntegerType *bitsType(LLVMContext &C, const Type *T) {
  return IntegerType::get(C, T->getPrimitiveSizeInBits());
}

Value *SquallLowerFloat::call(IRBuilder<> &B, StringRef Name, Type *Result,
                              ArrayRef<Value *> Args) {
  SmallVector<Type *, 2> Params;
  for (Value *A : Args)
    Params.push_back(A->getType());
  FunctionType *Type = FunctionType::get(Result, Params, false);
  if (Function *F = M->getFunction(Name); F && F->getFunctionType() == Type)
    return B.CreateCall(F, Args);
  return B.CreateCall(M->getOrInsertFunction(Name, Type), Args);
}

// compiler-rt comparisons return an integer ordered like the operands; for
// unordered operands __lesf2/__ltsf2/__eqsf2/__nesf2 return 1 and
// __gesf2/__gtsf2 return -1, so each predicate is one call and one integer
// comparison, or two for ONE and UEQ.
Value *SquallLowerFloat::lowerCompare(IRBuilder<> &B, CmpInst::Predicate P,
                                      Value *L, Value *R) {
  Type *I32 = B.getInt32Ty();
  std::string S = suffix(L->getType()).str();
  auto Cmp = [&](StringRef Base, CmpInst::Predicate IP) {
    Value *V = call(B, ("__" + Base + S + "2").str(), I32, {L, R});
    return B.CreateICmp(IP, V, B.getInt32(0));
  };
  switch (P) {
  case CmpInst::FCMP_FALSE:
    return B.getFalse();
  case CmpInst::FCMP_TRUE:
    return B.getTrue();
  case CmpInst::FCMP_OEQ:
    return Cmp("eq", CmpInst::ICMP_EQ);
  case CmpInst::FCMP_UNE:
    return Cmp("ne", CmpInst::ICMP_NE);
  case CmpInst::FCMP_OLT:
    return Cmp("lt", CmpInst::ICMP_SLT);
  case CmpInst::FCMP_OLE:
    return Cmp("le", CmpInst::ICMP_SLE);
  case CmpInst::FCMP_OGT:
    return Cmp("gt", CmpInst::ICMP_SGT);
  case CmpInst::FCMP_OGE:
    return Cmp("ge", CmpInst::ICMP_SGE);
  case CmpInst::FCMP_ULT: // Not OGE.
    return Cmp("ge", CmpInst::ICMP_SLT);
  case CmpInst::FCMP_ULE: // Not OGT.
    return Cmp("gt", CmpInst::ICMP_SLE);
  case CmpInst::FCMP_UGT: // Not OLE.
    return Cmp("le", CmpInst::ICMP_SGT);
  case CmpInst::FCMP_UGE: // Not OLT.
    return Cmp("lt", CmpInst::ICMP_SGE);
  case CmpInst::FCMP_UNO:
    return Cmp("unord", CmpInst::ICMP_NE);
  case CmpInst::FCMP_ORD:
    return Cmp("unord", CmpInst::ICMP_EQ);
  case CmpInst::FCMP_ONE:
    return B.CreateAnd(Cmp("unord", CmpInst::ICMP_EQ),
                       Cmp("ne", CmpInst::ICMP_NE));
  case CmpInst::FCMP_UEQ:
    return B.CreateOr(Cmp("unord", CmpInst::ICMP_NE),
                      Cmp("eq", CmpInst::ICMP_EQ));
  default:
    llvm_unreachable("not a floating-point predicate");
  }
}

// llvm.is.fpclass on the IEEE encoding: sign, then the magnitude ordered
// zero < subnormal < normal < infinity < quiet/signaling NaN.
Value *SquallLowerFloat::lowerClassTest(IRBuilder<> &B, Value *V,
                                        FPClassTest Mask) {
  Type *T = V->getType();
  IntegerType *IT = bitsType(B.getContext(), T);
  unsigned Bits = IT->getBitWidth(), Mantissa = T->isFloatTy() ? 23 : 52;
  APInt SignBit = APInt::getSignMask(Bits);
  APInt Inf = APInt::getBitsSet(Bits, Mantissa, Bits - 1);
  APInt Quiet = APInt::getOneBitSet(Bits, Mantissa - 1);
  Value *U = B.CreateBitCast(V, IT);
  Value *Abs = B.CreateAnd(U, ConstantInt::get(IT, ~SignBit));
  Value *Negative = B.CreateICmpNE(
      B.CreateAnd(U, ConstantInt::get(IT, SignBit)), ConstantInt::get(IT, 0));
  auto ULT = [&](const APInt &C) {
    return B.CreateICmpULT(Abs, ConstantInt::get(IT, C));
  };
  auto EQ = [&](const APInt &C) {
    return B.CreateICmpEQ(Abs, ConstantInt::get(IT, C));
  };
  APInt MinNormal = APInt::getOneBitSet(Bits, Mantissa);
  Value *Result = B.getFalse();
  auto Add = [&](Value *Class) { Result = B.CreateOr(Result, Class); };
  if (Mask & fcSNan)
    Add(B.CreateAnd(B.CreateICmpUGT(Abs, ConstantInt::get(IT, Inf)),
                    ULT(Inf | Quiet)));
  if (Mask & fcQNan)
    Add(B.CreateICmpUGE(Abs, ConstantInt::get(IT, Inf | Quiet)));
  auto Signed = [&](FPClassTest Neg, FPClassTest Pos, Value *Class) {
    if ((Mask & Neg) && (Mask & Pos))
      Add(Class);
    else if (Mask & Neg)
      Add(B.CreateAnd(Negative, Class));
    else if (Mask & Pos)
      Add(B.CreateAnd(B.CreateNot(Negative), Class));
  };
  Signed(fcNegInf, fcPosInf, EQ(Inf));
  Signed(fcNegNormal, fcPosNormal,
         B.CreateAnd(B.CreateICmpUGE(Abs, ConstantInt::get(IT, MinNormal)),
                     ULT(Inf)));
  Signed(fcNegSubnormal, fcPosSubnormal,
         B.CreateAnd(B.CreateICmpNE(Abs, ConstantInt::get(IT, 0)),
                     ULT(MinNormal)));
  Signed(fcNegZero, fcPosZero, EQ(APInt(Bits, 0)));
  return Result;
}

bool SquallLowerFloat::lower(Instruction &I) {
  IRBuilder<> B(&I);
  LLVMContext &C = I.getContext();
  Value *New = nullptr;
  Type *Ty = I.getType();

  if (auto *BO = dyn_cast<BinaryOperator>(&I); BO && isSoftType(Ty)) {
    StringRef Op;
    switch (BO->getOpcode()) {
    case Instruction::FAdd:
      Op = "add";
      break;
    case Instruction::FSub:
      Op = "sub";
      break;
    case Instruction::FMul:
      Op = "mul";
      break;
    case Instruction::FDiv:
      Op = "div";
      break;
    case Instruction::FRem:
      New = call(B, Ty->isFloatTy() ? "fmodf" : "fmod", Ty,
                 {BO->getOperand(0), BO->getOperand(1)});
      break;
    default:
      return false;
    }
    if (!New)
      New = call(B, ("__" + Op + suffix(Ty) + "3").str(), Ty,
                 {BO->getOperand(0), BO->getOperand(1)});
  } else if (auto *U = dyn_cast<UnaryOperator>(&I);
             U && U->getOpcode() == Instruction::FNeg && isSoftType(Ty)) {
    IntegerType *IT = bitsType(C, Ty);
    New = B.CreateBitCast(
        B.CreateXor(
            B.CreateBitCast(U->getOperand(0), IT),
            ConstantInt::get(IT, APInt::getSignMask(IT->getBitWidth()))),
        Ty);
  } else if (auto *F = dyn_cast<FCmpInst>(&I);
             F && isSoftType(F->getOperand(0)->getType())) {
    New =
        lowerCompare(B, F->getPredicate(), F->getOperand(0), F->getOperand(1));
  } else if (auto *Cast = dyn_cast<CastInst>(&I)) {
    Type *From = Cast->getSrcTy();
    Value *Src = Cast->getOperand(0);
    switch (Cast->getOpcode()) {
    case Instruction::FPExt:
      if (!From->isFloatTy() || !Ty->isDoubleTy())
        return false;
      New = call(B, "__extendsfdf2", Ty, {Src});
      break;
    case Instruction::FPTrunc:
      if (!From->isDoubleTy() || !Ty->isFloatTy())
        return false;
      New = call(B, "__truncdfsf2", Ty, {Src});
      break;
    case Instruction::FPToSI:
    case Instruction::FPToUI: {
      if (!isSoftType(From) || !Ty->isIntegerTy())
        return false;
      bool Signed = Cast->getOpcode() == Instruction::FPToSI;
      bool Wide = Ty->getIntegerBitWidth() > 32;
      if (Ty->getIntegerBitWidth() > 64)
        return false;
      Type *Via = Wide ? B.getInt64Ty() : B.getInt32Ty();
      // Narrower results convert through i32; out-of-range values are
      // poison either way.
      Value *V = call(B,
                      (Twine("__fix") + (Signed ? "" : "uns") + suffix(From) +
                       (Wide ? "di" : "si"))
                          .str(),
                      Via, {Src});
      New = B.CreateTrunc(V, Ty);
      break;
    }
    case Instruction::SIToFP:
    case Instruction::UIToFP: {
      if (!isSoftType(Ty) || From->getIntegerBitWidth() > 64)
        return false;
      bool Signed = Cast->getOpcode() == Instruction::SIToFP;
      bool Wide = From->getIntegerBitWidth() > 32;
      Type *Via = Wide ? B.getInt64Ty() : B.getInt32Ty();
      Value *V = Signed ? B.CreateSExt(Src, Via) : B.CreateZExt(Src, Via);
      New = call(B,
                 (Twine("__float") + (Signed ? "" : "un") +
                  (Wide ? "di" : "si") + suffix(Ty))
                     .str(),
                 Ty, {V});
      break;
    }
    default:
      return false;
    }
  } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    Type *T = II->getNumOperands() > 1 ? II->getArgOperand(0)->getType() : Ty;
    if (!isSoftType(T))
      return false;
    IntegerType *IT = bitsType(C, T);
    APInt Sign = APInt::getSignMask(IT->getBitWidth());
    auto Bits = [&](Value *V) { return B.CreateBitCast(V, IT); };
    auto Library = [&](StringRef Double) {
      std::string Name = Double.str();
      if (T->isFloatTy())
        Name += 'f';
      SmallVector<Value *, 2> Args(II->args());
      return call(B, Name, Ty, Args);
    };
    switch (II->getIntrinsicID()) {
    case Intrinsic::fabs:
      New = B.CreateBitCast(
          B.CreateAnd(Bits(II->getArgOperand(0)), ConstantInt::get(IT, ~Sign)),
          T);
      break;
    case Intrinsic::copysign:
      New = B.CreateBitCast(B.CreateOr(B.CreateAnd(Bits(II->getArgOperand(0)),
                                                   ConstantInt::get(IT, ~Sign)),
                                       B.CreateAnd(Bits(II->getArgOperand(1)),
                                                   ConstantInt::get(IT, Sign))),
                            T);
      break;
    case Intrinsic::is_fpclass:
      New = lowerClassTest(
          B, II->getArgOperand(0),
          FPClassTest(cast<ConstantInt>(II->getArgOperand(1))->getZExtValue()));
      break;
    case Intrinsic::fmuladd: {
      // Fusing is optional, so a separately rounded multiply and add do;
      // later expansion would need builtins this pass may have deleted.
      Value *Product = call(B, ("__mul" + suffix(T) + "3").str(), T,
                            {II->getArgOperand(0), II->getArgOperand(1)});
      New = call(B, ("__add" + suffix(T) + "3").str(), T,
                 {Product, II->getArgOperand(2)});
      break;
    }
    case Intrinsic::canonicalize:
    case Intrinsic::arithmetic_fence:
      New = II->getArgOperand(0);
      break;
    case Intrinsic::sqrt:
      New = Library("sqrt");
      break;
    case Intrinsic::floor:
      New = Library("floor");
      break;
    case Intrinsic::ceil:
      New = Library("ceil");
      break;
    case Intrinsic::trunc:
      New = Library("trunc");
      break;
    case Intrinsic::round:
      New = Library("round");
      break;
    case Intrinsic::minnum:
      New = Library("fmin");
      break;
    case Intrinsic::maxnum:
      New = Library("fmax");
      break;
    default:
      return false; // The IR checker reports the rest.
    }
  } else {
    return false;
  }
  New->takeName(&I);
  I.replaceAllUsesWith(New);
  I.eraseFromParent();
  return true;
}

// Runtime functions that LTO kept for code generation, and that nothing
// calls now, would only take space in the image; so would the internal
// helpers and variables that only they used.
bool SquallLowerFloat::stripUnusedRuntime() {
  bool Changed = false;
  for (bool Progress = true; Progress;) {
    Progress = false;
    SmallVector<GlobalValue *, 32> Kept;
    collectUsedGlobalVariables(*M, Kept, /*CompilerUsed=*/true);
    SmallPtrSet<Constant *, 32> Unused;
    for (GlobalValue *GV : Kept)
      if (auto *F = dyn_cast<Function>(GV);
          F && !F->isDeclaration() && F->hasOneUse())
        Unused.insert(F); // Only llvm.compiler.used refers to it.
    if (!Unused.empty())
      removeFromUsedLists(*M, [&](Constant *C) {
        return Unused.contains(C->stripPointerCasts());
      });
    for (Function &F : make_early_inc_range(*M)) {
      F.removeDeadConstantUsers();
      if (F.use_empty() && !F.isDeclaration() &&
          (F.hasLocalLinkage() || Unused.contains(&F))) {
        F.eraseFromParent();
        Progress = Changed = true;
      }
    }
    // State of deleted functions, such as the C library's rand seed.
    for (GlobalVariable &G : make_early_inc_range(M->globals())) {
      G.removeDeadConstantUsers();
      if (G.use_empty() && G.hasLocalLinkage()) {
        G.eraseFromParent();
        Progress = Changed = true;
      }
    }
  }
  return Changed;
}

bool SquallLowerFloat::runOnModule(Module &Mod) {
  M = &Mod;
  bool Changed = false;
  for (Function &F : Mod) {
    SmallVector<Instruction *, 32> Work;
    for (Instruction &I : instructions(F))
      Work.push_back(&I);
    for (Instruction *I : Work)
      Changed |= lower(*I);
  }
  // compiler-rt defines some builtins as aliases (__eqsf2 of __lesf2, for
  // example), which the calls above may name. A Squall image has no symbols,
  // so calls go to the aliasee.
  for (GlobalAlias &A : make_early_inc_range(Mod.aliases()))
    if (auto *F = dyn_cast<Function>(A.getAliasee()->stripPointerCasts())) {
      A.replaceAllUsesWith(F);
      A.eraseFromParent();
      Changed = true;
    }
  return stripUnusedRuntime() | Changed;
}
