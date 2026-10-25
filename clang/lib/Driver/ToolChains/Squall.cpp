//===--- Squall.cpp - Squall ToolChain Implementations ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A Squall object from `clang -c` is already a complete memory image for one
// module. Programs of several translation units are linked as LLVM bitcode:
// whenever the driver links, compilation produces bitcode, and the link step
// runs llvm-lto, which merges the modules, optimizes them together, and
// generates the image. Separately compiled objects for such a link come from
// `clang -c -flto`.
//
//===----------------------------------------------------------------------===//

#include "Squall.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Options/Options.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Option/ArgList.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

SquallToolChain::SquallToolChain(const Driver &D, const llvm::Triple &Triple,
                                 const ArgList &Args)
    : ToolChain(D, Triple, Args),
      Links(!Args.hasArg(options::OPT_c, options::OPT_S, options::OPT_E,
                         options::OPT_fsyntax_only, options::OPT_emit_llvm)) {
  // llvm-lto is installed beside clang.
  getProgramPaths().push_back(D.Dir);
}

Tool *SquallToolChain::buildLinker() const {
  return new tools::squall::Linker(*this);
}

void squall::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                  const InputInfo &Output,
                                  const InputInfoList &Inputs,
                                  const ArgList &Args,
                                  const char *LinkingOutput) const {
  const Driver &D = getToolChain().getDriver();
  ArgStringList CmdArgs;
  for (const InputInfo &Input : Inputs) {
    if (Input.isFilename()) {
      // An object from a plain `clang -c` is already a finished image.
      llvm::file_magic Magic;
      if (Input.getType() == types::TY_Object &&
          !llvm::identify_magic(Input.getFilename(), Magic) &&
          Magic != llvm::file_magic::bitcode)
        D.Diag(D.getDiags().getCustomDiagID(
            DiagnosticsEngine::Error,
            "'%0' is a complete Squall image, not bitcode; compile files to "
            "link with -flto"))
            << Input.getFilename();
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    // There are no libraries or linker scripts: the image is the program.
    D.Diag(diag::err_drv_unsupported_opt_for_target)
        << Input.getInputArg().getAsString(Args)
        << getToolChain().getTripleString();
  }
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Backend options, such as the entry function, apply at link time, when
  // the image is generated.
  StringRef Entry = "main";
  for (const Arg *A : Args.filtered(options::OPT_mllvm)) {
    A->claim();
    for (const char *Value : A->getValues()) {
      CmdArgs.push_back(Value);
      StringRef Option(Value);
      if (Option.consume_front("-squall-entry="))
        Entry = Option;
    }
  }
  // Everything but the entry function may be internalized and inlined.
  CmdArgs.push_back(Args.MakeArgString("-exported-symbol=" + Entry));
  unsigned Level = 0;
  if (Args.getLastArg(options::OPT_O_Group))
    Level = getOptimizationLevel(Args, InputKind(), D.getDiags());
  CmdArgs.push_back(Args.MakeArgString("-O" + Twine(Level)));

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(),
      Args.MakeArgString(getToolChain().GetProgramPath(getShortName())),
      CmdArgs, Inputs, Output));
}
