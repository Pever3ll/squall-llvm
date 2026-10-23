// SquallMCAsmInfo.h
#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCASMINFO_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {
class Triple;

class SquallMCAsmInfo : public MCAsmInfoELF {
public:
  explicit SquallMCAsmInfo(const Triple &TT);
};
} // namespace llvm

#endif
