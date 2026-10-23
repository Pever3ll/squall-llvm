#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLFIXUPKINDS_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace Squall {

enum Fixups {
  // d.off, Inst{62:43}, 20-bit signed word offset from ip.
  fixup_squall_doff = FirstTargetFixupKind,
  // d2.off, Inst{21:2}, 20-bit signed word offset (Extract).
  fixup_squall_d2off,
  // addr, Inst{38:28}, 11-bit slot address (signedness depends on `at`).
  fixup_squall_addr,
  // imm, Inst{41:10}, 32-bit literal.
  fixup_squall_imm32,
  // ptr, Inst{30:2}, 29-bit absolute word index.
  fixup_squall_ptr29,

  fixup_squall_invalid,
  NumTargetFixupKinds = fixup_squall_invalid - FirstTargetFixupKind
};

} // namespace Squall
} // namespace llvm

#endif
