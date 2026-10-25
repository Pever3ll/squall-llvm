//===--- Squall.h - Squall ToolChain Implementations ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_SQUALL_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_SQUALL_H

#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace tools {
namespace squall {

/// Links bitcode modules into one Squall image with llvm-lto: a Squall image
/// is a complete program, so linking happens before code generation.
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("squall::Linker", "llvm-lto", TC) {}
  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // namespace squall
} // namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY SquallToolChain : public ToolChain {
  bool Links; // Whether this invocation links, so it compiles to bitcode.

public:
  SquallToolChain(const Driver &D, const llvm::Triple &Triple,
                  const llvm::opt::ArgList &Args);

  bool IsIntegratedAssemblerDefault() const override { return true; }
  bool isCrossCompiling() const override { return true; }
  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &) const override { return false; }
  bool isPICDefaultForced() const override { return false; }
  bool SupportsProfiling() const override { return false; }
  bool HasNativeLLVMSupport() const override { return true; }
  LTOKind getDefaultLTOMode() const override {
    return Links ? LTOK_Full : LTOK_None;
  }

protected:
  Tool *buildLinker() const override;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_SQUALL_H
