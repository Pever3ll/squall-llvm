// SquallRegisterInfo.h
#ifndef SQUALL_REGISTER_INFO_H
#define SQUALL_REGISTER_INFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"
#define GET_REGINFO_HEADER
#define GET_REGINFO_ENUM
#include "SquallGenRegisterInfo.inc"

namespace llvm {

class SquallRegisterInfo : public SquallGenRegisterInfo {
public:
  SquallRegisterInfo() = default;

  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *) const override {
    return nullptr;
  }

  const uint32_t *getCallPreservedMask(const MachineFunction &,
                                       CallingConv::ID) const override {
    return nullptr;
  }
  BitVector getReservedRegs(const MachineFunction &) const override {
    return BitVector();
  }
};

} // namespace llvm
#endif
