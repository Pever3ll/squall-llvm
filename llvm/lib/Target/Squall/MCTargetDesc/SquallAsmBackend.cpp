// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "SquallMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
namespace {
class Writer : public MCELFObjectTargetWriter {
public:
  Writer()
      : MCELFObjectTargetWriter(false, ELF::ELFOSABI_STANDALONE, ELF::EM_SQUALL,
                                true) {}
  unsigned getRelocType(const MCFixup &F, const MCValue &,
                        bool) const override {
    switch (F.getKind()) {
    case Squall::Dest20:
      return ELF::R_SQUALL_BRANCH20;
    case Squall::Target20:
      return ELF::R_SQUALL_TARGET20;
    case Squall::Code11:
      return ELF::R_SQUALL_CODE11;
    case FK_Data_4:
      return ELF::R_SQUALL_32;
    case FK_Data_8:
      return ELF::R_SQUALL_64;
    default:
      report_fatal_error("unsupported Squall ELF relocation", false);
    }
  }
};
class Backend : public MCAsmBackend {
public:
  Backend() : MCAsmBackend(llvm::endianness::little) {}
  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return std::make_unique<Writer>();
  }
  MCFixupKindInfo getFixupKindInfo(MCFixupKind K) const override {
    static const MCFixupKindInfo Info[] = {{"fixup_squall_dest20", 43, 20, 0},
                                           {"fixup_squall_target20", 2, 20, 0},
                                           {"fixup_squall_code11", 28, 11, 0}};
    return K < FirstTargetFixupKind ? MCAsmBackend::getFixupKindInfo(K)
                                    : Info[K - FirstTargetFixupKind];
  }
  std::optional<bool> evaluateFixup(const MCFragment &F, MCFixup &Fix,
                                    MCValue &T, uint64_t &V) override {
    if (Fix.getKind() >= FirstTargetFixupKind && Fix.isPCRel() &&
        T.isAbsolute()) {
      V = T.getConstant() - Asm->getFragmentOffset(F) - Fix.getOffset();
      return true;
    }
    return {};
  }
  void applyFixup(const MCFragment &F, const MCFixup &Fix, const MCValue &T,
                  uint8_t *Data, uint64_t V, bool Resolved) override {
    maybeAddReloc(F, Fix, T, V, Resolved);
    auto Info = getFixupKindInfo(Fix.getKind());
    if (Fix.getKind() >= FirstTargetFixupKind) {
      if (Resolved && ((V & 7) || !isIntN(Info.TargetSize, int64_t(V) / 8)))
        getContext().reportError(Fix.getLoc(),
                                 "Squall relative address must be eight-byte "
                                 "aligned and fit its signed word offset");
      V = uint64_t(int64_t(V) / 8) & ((UINT64_C(1) << Info.TargetSize) - 1);
    }
    V <<= Info.TargetOffset;
    unsigned N = (Info.TargetSize + Info.TargetOffset + 7) / 8;
    for (unsigned I = 0; I < N; ++I)
      Data[I] |= uint8_t(V >> (8 * I));
  }
  bool writeNopData(raw_ostream &O, uint64_t Count,
                    const MCSubtargetInfo *) const override {
    if (Count % 8)
      return false;
    O.write_zeros(Count);
    return true;
  }
};
} // namespace
MCAsmBackend *llvm::createSquallAsmBackend(const Target &,
                                           const MCSubtargetInfo &,
                                           const MCRegisterInfo &,
                                           const MCTargetOptions &) {
  return new Backend();
}
