#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCTARGETDESC_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCTARGETDESC_H

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCTargetOptions.h"

#include "llvm/Support/DataTypes.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {
class MCAsmInfo;
class MCRegisterInfo;
class MCTargetAsmParser;
class MCAsmParser;
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

MCAsmInfo *createSquallMCAsmInfo(const MCRegisterInfo &MRI, const Triple &TT,
                                 const MCTargetOptions &Options);

MCInstPrinter *createSquallMCInstPrinter(const Triple &T,
                                         unsigned SyntaxVariant,
                                         const MCAsmInfo &MAI,
                                         const MCInstrInfo &MII,
                                         const MCRegisterInfo &MRI);

MCTargetAsmParser *createSquallMCAsmParser(const MCSubtargetInfo &STI,
                                           MCAsmParser &P,
                                           const MCInstrInfo &MII);
} // namespace llvm

#endif
