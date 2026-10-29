// SquallInstPrinter.h
#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLINSTPRINTER_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLINSTPRINTER_H

#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/MC/MCInstPrinter.h"

namespace llvm {

class SquallInstPrinter : public MCInstPrinter {
public:
  SquallInstPrinter(const MCAsmInfo &MAI, const MCInstrInfo &MII,
                    const MCRegisterInfo &MRI)
      : MCInstPrinter(MAI, MII, MRI) {}

  void printInst(const MCInst *MI, uint64_t Address, StringRef Annot,
                 const MCSubtargetInfo &STI, raw_ostream &O) override;
  std::pair<const char *, uint64_t>
  getMnemonic(const MCInst &MI) const override;

  void printInstruction(const MCInst *MI, uint64_t Address,
                        const MCSubtargetInfo &STI, raw_ostream &O);
  bool printAliasInstr(const MCInst *MI, uint64_t Address,
                       const MCSubtargetInfo &STI, raw_ostream &O);
  static const char *getRegisterName(MCRegister Reg);

  // Custom PrintMethods.
  void printPort(const MCInst *MI, unsigned OpNo, const MCSubtargetInfo &STI,
                 raw_ostream &O);
  void printSecondPort(const MCInst *MI, unsigned OpNo,
                       const MCSubtargetInfo &STI, raw_ostream &O);
  void printIDest(const MCInst *MI, unsigned OpNo, const MCSubtargetInfo &STI,
                  raw_ostream &O);
  void printFDest(const MCInst *MI, unsigned OpNo, const MCSubtargetInfo &STI,
                  raw_ostream &O);
  void printSImm32(const MCInst *MI, unsigned OpNo, const MCSubtargetInfo &STI,
                   raw_ostream &O);
  void printPtr(const MCInst *MI, unsigned OpNo, const MCSubtargetInfo &STI,
                raw_ostream &O);
};

} // namespace llvm
#endif
