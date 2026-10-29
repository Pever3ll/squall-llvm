#include "MCTargetDesc/SquallELFObjectWriter.h"
#include "MCTargetDesc/SquallFixupKinds.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>

using namespace llvm;

class SquallELFObjectWriter : public MCELFObjectTargetWriter {
public:
  SquallELFObjectWriter(const Triple &TT)
      : MCELFObjectTargetWriter(true, getOSABI(TT.getOS()), ELF::EM_NONE,
                                true) {}

  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const {
    return 0;
  }
};

std::unique_ptr<MCObjectTargetWriter>
llvm::createSquallELFObjectWriter(const Triple &TT) {
  return std::unique_ptr<MCObjectTargetWriter>(new SquallELFObjectWriter(TT));
}
