// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "SquallTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getTheSquallTarget() {
  static Target TheTarget;
  return TheTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeSquallTargetInfo() {
  RegisterTarget<Triple::squall> X(getTheSquallTarget(), "squall",
                                   "Squall v2 explicit token store", "Squall");
}
