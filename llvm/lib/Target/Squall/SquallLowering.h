// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_TARGET_SQUALL_LOWERING_H
#define LLVM_LIB_TARGET_SQUALL_LOWERING_H
#include "llvm/Support/CodeGen.h"
namespace llvm {
class ModulePass;
class TargetMachine;
class raw_pwrite_stream;
ModulePass *createSquallLoweringPass(TargetMachine &, raw_pwrite_stream &,
                                     CodeGenFileType);
} // namespace llvm
#endif
