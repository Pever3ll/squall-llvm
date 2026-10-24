// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_H
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#define GET_REGINFO_ENUM
#include "SquallGenRegisterInfo.inc"
#define GET_INSTRINFO_ENUM
#include "SquallGenInstrInfo.inc"
namespace llvm {
class MCContext;
namespace Squall {
enum Fixups { Dest20 = FirstTargetFixupKind, Target20, Code11 };
const char *aluName(unsigned Op);
int aluCode(StringRef Name);
// Resolves instruction fields and reports malformed MCInsts without truncation.
uint64_t encode(const MCInst &, SmallVectorImpl<MCFixup> &, MCContext &);
} // namespace Squall
class MCAsmBackend;
class MCSubtargetInfo;
class MCRegisterInfo;
class MCTargetOptions;
class Target;
MCAsmBackend *createSquallAsmBackend(const Target &, const MCSubtargetInfo &,
                                     const MCRegisterInfo &,
                                     const MCTargetOptions &);
} // namespace llvm
#endif
