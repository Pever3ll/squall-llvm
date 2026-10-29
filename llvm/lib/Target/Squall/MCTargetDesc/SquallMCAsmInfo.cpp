// SquallMCAsmInfo.cpp
#include "SquallMCAsmInfo.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include <llvm/MC/MCRegisterInfo.h>

using namespace llvm;

SquallMCAsmInfo::SquallMCAsmInfo(const Triple &TT,
                                 const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  CodePointerSize = 8;
  CalleeSaveStackSlotSize = 8;
  IsLittleEndian = true;
  CommentString = ";";
  SupportsDebugInformation = false;
  ExceptionsType = ExceptionHandling::None;
  MinInstAlignment = 8;
  AllowAtInName = false;
}

MCAsmInfo *llvm::createSquallMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT,
                                       const MCTargetOptions &Options) {
  return new SquallMCAsmInfo(TT, Options);
}
