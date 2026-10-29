#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLFIXUPKINDS_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace Squall {

enum Fixups {
  // d.off, Inst{62:43}, 20-bit signed word offset from ip.
  fixup_squall_doff_rel = FirstTargetFixupKind,
  fixup_squall_doff_abs,

  // d2.off, Inst{21:2}, 20-bit signed word offset (Extract).
  fixup_squall_d2off_rel,
  fixup_squall_d2off_abs,

  // addr, Inst{38:28}, 11-bit; signedness depends on at.
  fixup_squall_addr_frame, // unsigned
  fixup_squall_addr_code,  // signed, ip-relative
  fixup_squall_addr_abs,   // unsigned

  // imm, Inst{41:10}, 32-bit.
  fixup_squall_imm32,

  // ptr, Inst{30:2}, 29-bit unsigned absolute word index.
  fixup_squall_ptr29,

  fixup_squall_invalid,
  NumTargetFixupKinds = fixup_squall_invalid - FirstTargetFixupKind
};

} // namespace Squall
} // namespace llvm

#endif
