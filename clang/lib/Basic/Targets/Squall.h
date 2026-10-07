
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

    resetDataLayout("e-m:e-p:32:32-i64:64-n32:64-S64"); // does something idk
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;
};

} // namespace targets
} // namespace clang

#endif
