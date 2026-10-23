// SquallInstPrinter.cpp
#include "SquallInstPrinter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
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
  O << (MI->getOperand(OpNo).getImm() ? 'r' : 'l');
}

void SquallInstPrinter::printSecondPort(const MCInst *MI, unsigned OpNo,
                                        const MCSubtargetInfo &STI,
                                        raw_ostream &O) {
  int64_t V = MI->getOperand(OpNo).getImm();
  if (V == 0)
    return; // absent
  O << ' ' << ((V & 1) ? 'r' : 'l');
}

void SquallInstPrinter::printIDest(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.is
