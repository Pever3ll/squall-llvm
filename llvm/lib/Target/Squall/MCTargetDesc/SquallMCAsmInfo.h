// SquallMCAsmInfo.h
#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCASMINFO_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCTargetOptions.h"

namespace llvm {
class Triple;

class SquallMCAsmInfo : public MCAsmInfoELF {
public:
  explicit SquallMCAsmInfo(const Triple &TT, const MCTargetOptions &Options);
};
} // namespace llvm

#endif
