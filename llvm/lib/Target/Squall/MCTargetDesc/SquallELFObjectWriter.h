#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLELFOBJECTWRITER_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLELFOBJECTWRITER_H

#include <memory>

namespace llvm {

class MCObjectTargetWriter;
class Triple;

std::unique_ptr<MCObjectTargetWriter>
createSquallELFObjectWriter(const Triple &TT);

} // namespace llvm

#endif
