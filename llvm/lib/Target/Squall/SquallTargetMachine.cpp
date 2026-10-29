//===-- SquallTargetMachine.cpp - Squall target machine -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallTargetMachine.h"
#include "Squall.h"
#include "SquallMachineFunctionInfo.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallTarget() {
  RegisterTargetMachine<SquallTargetMachine> X(getTheSquallTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeSquallAsmPrinterPass(PR);
  initializeSquallCheckIRPass(PR);
  initializeSquallLowerMemoryPass(PR);
  initializeSquallLowerFloatPass(PR);
  initializeSquallLowerAllocaPass(PR);
  initializeSquallForkJoinPass(PR);
  initializeSquallDAGToDAGISelLegacyPass(PR);
}

namespace {
/// Squall executes a single physical memory image: code, data, frames and the
/// token stack share one address space. Everything is emitted into one
/// section so that all references resolve without a linker.
class SquallTargetObjectFile : public TargetLoweringObjectFileELF {
  MCSection *image() const {
    return getContext().getELFSection(".squall.image", ELF::SHT_PROGBITS,
                                      ELF::SHF_ALLOC | ELF::SHF_WRITE |
                                          ELF::SHF_EXECINSTR);
  }

public:
  MCSection *getExplicitSectionGlobal(const GlobalObject *, SectionKind,
                                      const TargetMachine &) const override {
    return image();
  }
  MCSection *SelectSectionForGlobal(const GlobalObject *, SectionKind,
                                    const TargetMachine &) const override {
    return image();
  }
};

class SquallPassConfig : public TargetPassConfig {
public:
  SquallPassConfig(SquallTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  void addIRPasses() override {
    // Floating point becomes runtime calls, and dynamic allocas use the
    // alloca area, before the checker looks at what remains.
    addPass(createSquallLowerAllocaPass());
    addPass(createSquallLowerFloatPass());
    addPass(createSquallCheckIRPass());
    TargetPassConfig::addIRPasses();
  }
  // Runs after the generic IR passes, so nothing creates new partial-cell
  // stores afterwards.
  bool addPreISel() override {
    addPass(createSquallLowerMemoryPass());
    return false;
  }
  // After register allocation, before the frame layout is fixed.
  void addPostRegAlloc() override { addPass(createSquallForkJoinPass()); }
  bool addInstSelector() override {
    addPass(createSquallISelDag(getTM<SquallTargetMachine>(), getOptLevel()));
    return false;
  }
};
} // namespace

SquallTargetMachine::SquallTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         std::optional<Reloc::Model> RM,
                                         std::optional<CodeModel::Model> CM,
                                         CodeGenOptLevel OL, bool)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               RM.value_or(Reloc::Static),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<SquallTargetObjectFile>()),
      Subtarget(TT, CPU, FS, *this) {
  initAsmInfo();
}

SquallTargetMachine::~SquallTargetMachine() = default;

TargetPassConfig *SquallTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new SquallPassConfig(*this, PM);
}

MachineFunctionInfo *SquallTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return SquallMachineFunctionInfo::create<SquallMachineFunctionInfo>(Allocator,
                                                                      F, STI);
}
