#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCTARGETDESC_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCTARGETDESC_H

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCTargetOptions.h"

#include "llvm/Support/DataTypes.h"

namespace llvm {
class MCAsmInfo;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCInstPrinter;
class MCCodeEmitter;
class MCAsmBackend;
class MCObjectTargetWriter;
class MCContext;
class Target;
class MCTargetOptions;

MCCodeEmitter *createSquallMCCodeEmitter(const MCInstrInfo &MCII,
                                         MCContext &Ctx);

MCAsmBackend *createSquallMCAsmBackend(const Target &T,
                                       const MCSubtargetInfo &STI,
                                       const MCRegisterInfo &MRI,
                                       const MCTargetOptions &Options);
} // namespace llvm

#endif
