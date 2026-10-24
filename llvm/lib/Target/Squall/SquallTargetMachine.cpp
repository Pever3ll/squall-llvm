// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "SquallLowering.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

namespace {
// A token/slot machine lowers IR directly to MCInsts; no register allocator
// or external compiler process is involved.
class SquallTargetMachine final : public TargetMachine {
  mutable TargetLoweringObjectFileELF ObjectFile;

public:
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return &ObjectFile;
  }
  bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                           raw_pwrite_stream *, CodeGenFileType FileType, bool,
                           MachineModuleInfoWrapperPass *MMI) override {
    if (FileType == CodeGenFileType::Null)
      return true;
    if (MMI)
      PM.add(MMI);
    PM.add(createSquallLoweringPass(*this, Out, FileType));
    return false;
  }
  SquallTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                      StringRef Features, const TargetOptions &Options,
                      std::optional<Reloc::Model> Relocation,
                      std::optional<CodeModel::Model> Model,
                      CodeGenOptLevel Level, bool)
      : TargetMachine(T, TT.computeDataLayout(), TT, CPU, Features, Options) {
    RM = Relocation.value_or(Reloc::Static);
    CMModel = Model.value_or(CodeModel::Small);
    OptLevel = Level;
    MRI.reset(T.createMCRegInfo(TT));
    AsmInfo.reset(T.createMCAsmInfo(*MRI, TT, this->Options.MCOptions));
    MII.reset(T.createMCInstrInfo());
    STI.reset(T.createMCSubtargetInfo(TT, "", ""));
  }
};
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTarget() {
  RegisterTargetMachine<SquallTargetMachine> X(getTheSquallTarget());
}
