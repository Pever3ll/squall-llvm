// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

namespace {
// TargetMachine's default emission hooks report "unsupported". Do not advertise
// a register-based code generator until it exists; use squall-compile instead.
class SquallTargetMachine final : public TargetMachine {
public:
  SquallTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                     StringRef Features, const TargetOptions &Options,
                     std::optional<Reloc::Model> Relocation,
                     std::optional<CodeModel::Model> Model,
                     CodeGenOptLevel Level, bool)
      : TargetMachine(T, TT.computeDataLayout(), TT, CPU, Features, Options) {
    RM = Relocation.value_or(Reloc::Static);
    CMModel = Model.value_or(CodeModel::Small);
    OptLevel = Level;
    // llc constructs its MachineModuleInfo/MCContext before consulting the
    // emission hook. Supply empty metadata so unsupported native emission
    // produces the normal diagnostic instead of dereferencing null pointers.
    // These objects do not define instructions, registers, or an MC encoder.
    AsmInfo = std::make_unique<MCAsmInfo>(this->Options.MCOptions);
    MRI = std::make_unique<MCRegisterInfo>();
    STI = std::make_unique<MCSubtargetInfo>(
        TT, "", "", "", StringTable(""), ArrayRef<SubtargetFeatureKV>{},
        ArrayRef<SubtargetSubTypeKV>{}, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr);
  }
};
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTarget() {
  RegisterTargetMachine<SquallTargetMachine> X(getTheSquallTarget());
}
