// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "llvm/Support/Compiler.h"

// Discovery is supported; MC emission remains deliberately unavailable.
// Squall assembly/image generation currently lives in tools/squall-compiler.
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTargetMC() {}
