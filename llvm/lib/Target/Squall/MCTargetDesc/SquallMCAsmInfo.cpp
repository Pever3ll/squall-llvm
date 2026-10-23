// SquallMCAsmInfo.cpp
#include "SquallMCAsmInfo.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

SquallMCAsmInfo::SquallMCAsmInfo(const Triple &TT) {
  CodePointerSize = 8;
  CalleeSaveStackSlotSize = 8;
  IsLittleEndian = true;
  CommentString = ";";
  SupportsDebugInformation = false;
  ExceptionsType = ExceptionHandling::None;
  MinInstAlignment = 8;
  AllowAtInName = false;
}
