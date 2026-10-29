#include "MCTargetDesc/SquallELFObjectWriter.h"
#include "MCTargetDesc/SquallFixupKinds.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace {

class SquallAsmBackend : public MCAsmBackend {
public:
  SquallAsmBackend() : MCAsmBackend(llvm::endianness::little) {}

  unsigned getNumFixupKinds() const { return Squall::NumTargetFixupKinds; }

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override {
    // {name, TargetOffset, TargetSize, Flags}
    static const MCFixupKindInfo Infos[Squall::NumTargetFixupKinds] = {
        {"fixup_squall_doff_rel", 43, 20, 0},
        {"fixup_squall_doff_abs", 43, 20, 0},
        {"fixup_squall_d2off_rel", 2, 20, 0},
        {"fixup_squall_d2off_abs", 2, 20, 0},
        {"fixup_squall_addr_frame", 28, 11, 0},
        {"fixup_squall_addr_code", 28, 11, 0},
        {"fixup_squall_addr_abs", 28, 11, 0},
        {"fixup_squall_imm32", 10, 32, 0},
        {"fixup_squall_ptr29", 2, 29, 0}};
    // {FK_NONE, 0, 0}};

    if (Kind < FirstTargetFixupKind)
      return MCAsmBackend::getFixupKindInfo(Kind);
    return Infos[Kind - FirstTargetFixupKind];
  }

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  bool fixupNeedsRelaxationAdvanced(const MCFragment &, const MCFixup &,
                                    const MCValue &, uint64_t,
                                    bool) const override {
    return false;
  }

  bool fixupNeedsRelaxation(const MCFixup &Fixup, uint64_t Value) const {
    return false; // fixed 64-bit words
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createSquallELFObjectWriter(Triple::squall);
  }
};

} // namespace

void SquallAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                  const MCValue &Target, uint8_t *Data,
                                  uint64_t Value, bool IsResolved) {
  if (!Value)
    return;

  MCFixupKind Kind = Fixup.getKind();
  const MCFixupKindInfo &Info = getFixupKindInfo(Kind);

  // Offsets and d.off are in *words*, not bytes.
  switch (Kind) {
  case Squall::fixup_squall_doff_rel:
  case Squall::fixup_squall_d2off_rel:
  case Squall::fixup_squall_addr_code:
    Value = static_cast<int64_t>(Value) / 8;
    break;
  default:
    break;
  }

  uint64_t Mask = (1ULL << Info.TargetSize) - 1;
  uint64_t Insn = support::endian::read64le(Data + Fixup.getOffset());
  Insn |= (Value & Mask) << Info.TargetOffset;
  support::endian::write64le(Data + Fixup.getOffset(), Insn);
}

bool SquallAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                    const MCSubtargetInfo *STI) const {
  if (Count % 8 != 0)
    return false;
  // NOP is alu_op 0 in the monadic form, everything else zero.
  for (uint64_t I = 0; I != Count; I += 8)
    support::endian::write<uint64_t>(OS, 0, llvm::endianness::little);
  return true;
}

MCAsmBackend *llvm::createSquallMCAsmBackend(const Target &T,
                                             const MCSubtargetInfo &STI,
                                             const MCRegisterInfo &MRI,
                                             const MCTargetOptions &Options) {
  return new SquallAsmBackend();
}
