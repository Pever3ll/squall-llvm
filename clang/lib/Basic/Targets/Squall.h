
#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_SQUALL_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_SQUALL_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY SquallTargetInfo : public TargetInfo {

public:
  SquallTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    PointerAlign = 32;
    PointerWidth = 32;

    // C sizes are logical: the backend stores each four-byte cell of memory in
    // one 64-bit matching slot. Byte-sized memory accesses are rejected.
    TLSSupported = false;
    resetDataLayout(Triple.computeDataLayout());
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }
  BuiltinVaListKind getBuiltinVaListKind() const override {
    return CharPtrBuiltinVaList;
  }
  ArrayRef<const char *> getGCCRegNames() const override { return {}; }
  ArrayRef<GCCRegAlias> getGCCRegAliases() const override { return {}; }
  bool validateAsmConstraint(const char *&, ConstraintInfo &) const override {
    return false;
  }
  std::string_view getClobbers() const override { return ""; }
};

} // namespace targets
} // namespace clang

#endif
