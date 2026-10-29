#include "MCTargetDesc/SquallFixupKinds.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#define GET_INSTRINFO_ENUM
#include "SquallGenInstrInfo.inc"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/EndianStream.h"

using namespace llvm;

namespace {

class SquallMCCodeEmitter : public MCCodeEmitter {
  MCContext &Ctx;
  const MCInstrInfo &MCII;

public:
  SquallMCCodeEmitter(MCContext &Ctx, const MCInstrInfo &MCII)
      : Ctx(Ctx), MCII(MCII) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  // Generated.
  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  unsigned getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  // Custom EncoderMethods.
  uint64_t getIDestOpValue(const MCInst &MI, unsigned OpNo,
                           SmallVectorImpl<MCFixup> &Fixups,
                           const MCSubtargetInfo &STI) const;
  uint64_t getFDestOpValue(const MCInst &MI, unsigned OpNo,
                           SmallVectorImpl<MCFixup> &Fixups,
                           const MCSubtargetInfo &STI) const;
  uint64_t getImm32OpValue(const MCInst &MI, unsigned OpNo,
                           SmallVectorImpl<MCFixup> &Fixups,
                           const MCSubtargetInfo &STI) const;
  uint64_t getPtrOpValue(const MCInst &MI, unsigned OpNo,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const;
};

} // namespace

void SquallMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                            SmallVectorImpl<char> &CB,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
  support::endian::write<uint64_t>(CB, Bits, llvm::endianness::little);
}

// Default path for plain operands (port, second_port).
unsigned
SquallMCCodeEmitter::getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  llvm_unreachable("unexpected operand kind");
}

uint64_t
SquallMCCodeEmitter::getIDestOpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return MO.getImm() & 0xFFFFF; // 20 bits

  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   MCFixupKind(Squall::fixup_squall_doff_rel)));
  return 0;
}

// fdest packs {at[1:0], addr[10:0]} into 13 bits; see DyadicInstr.
uint64_t
SquallMCCodeEmitter::getFDestOpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &AddrMO = MI.getOperand(OpNo);
  const MCOperand &AtMO = MI.getOperand(OpNo + 1);

  assert(AtMO.isImm() && "addr_tag must be a literal");
  uint64_t At = AtMO.getImm() & 0x3;

  if (AddrMO.isImm())
    return (At << 11) | (AddrMO.getImm() & 0x7FF);

  Squall::Fixups Kind;
  switch (At) {
  case 0:
    Kind = Squall::fixup_squall_addr_frame;
    break;
  case 1:
    Kind = Squall::fixup_squall_addr_code;
    break;
  default:
    Kind = Squall::fixup_squall_addr_abs;
    break;
  }
  Fixups.push_back(MCFixup::create(0, AddrMO.getExpr(), MCFixupKind(Kind)));
  return At << 11;
}

uint64_t
SquallMCCodeEmitter::getImm32OpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint32_t>(MO.getImm());

  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   MCFixupKind(Squall::fixup_squall_imm32)));
  return 0;
}

uint64_t SquallMCCodeEmitter::getPtrOpValue(const MCInst &MI, unsigned OpNo,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm()) & 0x1FFFFFFF;

  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   MCFixupKind(Squall::fixup_squall_ptr29)));
  return 0;
}

MCCodeEmitter *llvm::createSquallMCCodeEmitter(const MCInstrInfo &MCII,
                                               MCContext &Ctx) {
  return new SquallMCCodeEmitter(Ctx, MCII);
}

#include "SquallGenMCCodeEmitter.inc"
