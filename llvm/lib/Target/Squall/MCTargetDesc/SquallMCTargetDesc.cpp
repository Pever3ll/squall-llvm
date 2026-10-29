// SquallMCTargetDesc.cpp

#include "SquallMCTargetDesc.h"

#include "SquallInstPrinter.h"
#include "SquallMCAsmInfo.h"
#include "SquallRegisterInfo.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace llvm {
namespace Squall {
using ::llvm::SquallRegClassRegClassID;
}
} // namespace llvm

#define GET_REGINFO_MC_DESC
#include "SquallGenRegisterInfo.inc"

#define GET_INSTRINFO_MC_DESC
#include "SquallGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "SquallGenSubtargetInfo.inc"

static MCInstrInfo *createSquallMCInstrInfo() {
  auto *X = new MCInstrInfo();
  InitSquallMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createSquallMCRegisterInfo(const Triple &TT) {
  auto *X = new MCRegisterInfo();
  InitSquallMCRegisterInfo(X, llvm::NoRegister);
  return X;
}

static MCSubtargetInfo *
createSquallMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  return createSquallMCSubtargetInfoImpl(TT, CPU, CPU, FS);
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTargetMC() {
  Target &T = getTheSquallTarget();

  TargetRegistry::RegisterMCInstrInfo(T, createSquallMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createSquallMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createSquallMCSubtargetInfo);
  TargetRegistry::RegisterMCAsmInfo(T, createSquallMCAsmInfo);
  TargetRegistry::RegisterMCCodeEmitter(T, createSquallMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createSquallMCAsmBackend);
  TargetRegistry::RegisterMCInstPrinter(T, createSquallMCInstPrinter);
  TargetRegistry::RegisterMCAsmParser(T, createSquallMCAsmParser);
}
