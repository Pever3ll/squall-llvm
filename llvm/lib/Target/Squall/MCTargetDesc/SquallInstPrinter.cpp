// SquallInstPrinter.cpp
#include "SquallInstPrinter.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define PRINT_ALIAS_INSTR
#include "SquallGenAsmWriter.inc"

void SquallInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                  StringRef Annot, const MCSubtargetInfo &STI,
                                  raw_ostream &O) {
  printInstruction(MI, Address, STI, O);
  printAnnotation(O, Annot);
}

void SquallInstPrinter::printPort(const MCInst *MI, unsigned OpNo,
                                  const MCSubtargetInfo &STI, raw_ostream &O) {
  O << '.' << (MI->getOperand(OpNo).getImm() ? 'r' : 'l');
}

void SquallInstPrinter::printSecondPort(const MCInst *MI, unsigned OpNo,
                                        const MCSubtargetInfo &STI,
                                        raw_ostream &O) {
  int64_t V = MI->getOperand(OpNo).getImm();
  if (V == 0)
    return;                          // absent: emit nothing
  O << ' ' << ((V & 1) ? 'r' : 'l'); // 2 -> " l", 3 -> " r"
}

void SquallInstPrinter::printIDest(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm()) {
    O << '(' << SignExtend64<20>(MO.getImm()) << ')';
    return;
  }
  MO.print(O);
}

// fdest is two MCOperands: addr, then at.
void SquallInstPrinter::printFDest(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &AddrMO = MI->getOperand(OpNo);
  const MCOperand &AtMO = MI->getOperand(OpNo + 1);
  int64_t At = AtMO.getImm();

  const char *Open = At == 0 ? "[" : At == 1 ? "(" : "";
  const char *Close = At == 0 ? "]" : At == 1 ? ")" : "";

  O << Open;
  if (AddrMO.isImm()) {
    if (At == 1)
      O << SignExtend64<11>(AddrMO.getImm());
    else
      O << static_cast<uint64_t>(AddrMO.getImm());
  } else {
    AddrMO.print(O);
  }
  O << Close;
}

void SquallInstPrinter::printSImm32(const MCInst *MI, unsigned OpNo,
                                    const MCSubtargetInfo &STI,
                                    raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    O << MO.getImm();
  else
    MO.print(O);
}

void SquallInstPrinter::printPtr(const MCInst *MI, unsigned OpNo,
                                 const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    O << format_hex(static_cast<uint64_t>(MO.getImm()), 2);
  else
    MO.print(O);
}

MCInstPrinter *llvm::createSquallMCInstPrinter(const Triple &T,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  return new SquallInstPrinter(MAI, MII, MRI);
}
