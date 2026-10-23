#include "MCTargetDesc/SquallFixupKinds.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
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

  // Default for plain operands (port, second_port).
  uint64_t getMachineOpValue(const MCInst &MI, const MCOperand &MO,
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

uint64_t
SquallMCCodeEmitter::getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm());
  llvm_unreachable("unexpected operand kind in getMachineOpValue");
}

uint64_t
SquallMCCodeEmitter::getIDestOpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm()) & 0xFFFFF; // 20 bits

  // Which field this lands in depends on the opcode; d2.off for Extract's
  // payload, d.off otherwise. OpNo distinguishes them: $dst is always
  // operand 0, $ipay is operand 2.
  Squall::Fixups Kind =
      (OpNo == 0) ? Squall::fixup_squall_doff : Squall::fixup_squall_d2off;

  Fixups.push_back(
      MCFixup::create(0, MO.getExpr(), MCFixupKind(Kind), MI.getLoc()));
  return 0;
}

uint64_t
SquallMCCodeEmitter::getFDestOpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &AddrOp = MI.getOperand(OpNo);
  int64_t Tag = MI.getOperand(OpNo + 1).getImm();

  uint64_t Addr = 0;
  if (AddrOp.isImm())
    Addr = static_cast<uint64_t>(AddrOp.getImm()) & 0x7FF; // 11 bits
  else
    Fixups.push_back(MCFixup::create(0, AddrOp.getExpr(),
                                     MCFixupKind(Squall::fixup_squall_addr),
                                     MI.getLoc()));

  // Packed as {at[1:0], addr[10:0]} to match DyadicInstr's `bits<13> slot`.
  return (static_cast<uint64_t>(Tag & 0x3) << 11) | Addr;
}

uint64_t
SquallMCCodeEmitter::getImm32OpValue(const MCInst &MI, unsigned OpNo,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint32_t>(MO.getImm());

  Fixups.push_back(MCFixup::create(
      0, MO.getExpr(), MCFixupKind(Squall::fixup_squall_imm32), MI.getLoc()));
  return 0;
}

uint64_t SquallMCCodeEmitter::getPtrOpValue(const MCInst &MI, unsigned OpNo,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm()) & 0x1FFFFFFF; // 29 bits

  Fixups.push_back(MCFixup::create(
      0, MO.getExpr(), MCFixupKind(Squall::fixup_squall_ptr29), MI.getLoc()));
  return 0;
}

MCCodeEmitter *llvm::createSquallMCCodeEmitter(const MCInstrInfo &MCII,
                                               MCContext &Ctx) {
  return new SquallMCCodeEmitter(Ctx, MCII);
}

#include "SquallGenMCCodeEmitter.inc"
