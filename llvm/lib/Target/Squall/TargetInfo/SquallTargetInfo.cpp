#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

Target &llvm::getTheSquallTarget() {
  static Target TheSquallTarget;
  return TheSquallTarget;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTargetInfo() {
  RegisterTarget<Triple::squall, /*HasJIT=*/false> X(
      getTheSquallTarget(), "squall",
      "Squall dataflow architecture based on MIT's Monsoon architecture",
      "Squall");
}
