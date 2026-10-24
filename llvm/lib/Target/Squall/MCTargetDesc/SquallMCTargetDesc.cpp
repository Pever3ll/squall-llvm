// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "SquallMCTargetDesc.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
#define GET_INSTRINFO_MC_DESC
#include "SquallGenInstrInfo.inc"
#define GET_REGINFO_MC_DESC
#include "SquallGenRegisterInfo.inc"
#define GET_SUBTARGETINFO_MC_DESC
#include "SquallGenSubtargetInfo.inc"
namespace {
const char *Names[35] = {
    "nop",  nullptr, nullptr, nullptr, "add",   "sub",   "mul",   "div", "divu",
    "rem",  "mod",   nullptr, nullptr, nullptr, nullptr, nullptr, "and", "or",
    "xor",  "shl",   "shrl",  "shra",  nullptr, nullptr, "not",   "rev", "dupl",
    "dupr", nullptr, nullptr, nullptr, nullptr, "beqz",  "beql",  "beqr"};
class SquallAsmInfo : public MCAsmInfoELF {
public:
  SquallAsmInfo(const MCTargetOptions &O) : MCAsmInfoELF(O) {
    CodePointerSize = 4;
    CalleeSaveStackSlotSize = 8;
    MinInstAlignment = 8;
    MaxInstLength = 8;
    CommentString = ";";
    Data64bitsDirective = "\t.quad\t";
    SupportsDebugInformation = false;
  }
};
class Emitter : public MCCodeEmitter {
  MCContext &Ctx;

public:
  Emitter(MCContext &C) : Ctx(C) {}
  void encodeInstruction(const MCInst &I, SmallVectorImpl<char> &B,
                         SmallVectorImpl<MCFixup> &F,
                         const MCSubtargetInfo &) const override {
    support::endian::write(B, Squall::encode(I, F, Ctx),
                           llvm::endianness::little);
  }
};
class Printer : public MCInstPrinter {
  void operand(const MCOperand &V, raw_ostream &O, bool Relative = false) {
    if (V.isImm()) {
      if (Relative)
        O << '(';
      O << V.getImm();
      if (Relative)
        O << ')';
    } else
      MAI.printExpr(O, *V.getExpr());
  }
  void dest(const MCInst *I, unsigned N, raw_ostream &O) {
    operand(I->getOperand(N), O, true);
    O << (I->getOperand(N + 1).getImm() ? ".r" : ".l");
  }

public:
  using MCInstPrinter::MCInstPrinter;
  std::pair<const char *, uint64_t>
  getMnemonic(const MCInst &I) const override {
    if (I.getOpcode() == Squall::WRITE)
      return {"write", 0};
    if (I.getOpcode() == Squall::SEND)
      return {"send", 0};
    if (I.getOpcode() == Squall::EXTRACT)
      return {"extract", 0};
    return {Squall::aluName(I.getOperand(0).getImm()), 0};
  }
  void printRegName(raw_ostream &, MCRegister) override {
    llvm_unreachable("Squall has no registers");
  }
  void printInst(const MCInst *I, uint64_t, StringRef A,
                 const MCSubtargetInfo &, raw_ostream &O) override {
    unsigned K = I->getOpcode();
    O << '\t' << getMnemonic(*I).first;
    if (K == Squall::WRITE) {
      O << ' ';
      operand(I->getOperand(0), O);
      printAnnotation(O, A);
      return;
    }
    unsigned D = K == Squall::ALUI || K == Squall::ALUS ? 1 : 0;
    unsigned S = K == Squall::ALUS || K == Squall::EXTRACT ? 4 : 2;
    if (K != Squall::ALUI) {
      int Rule = I->getOperand(S + 2).getImm();
      if (Rule)
        O << (Rule == 1 ? ".c" : ".r");
    }
    O << ' ';
    dest(I, D, O);
    if (D && I->getOperand(3).getImm() >= 0)
      O << (I->getOperand(3).getImm() ? ".r" : ".l");
    O << ' ';
    if (K == Squall::EXTRACT) {
      dest(I, 2, O);
      O << ' ';
    }
    if (K == Squall::ALUI) {
      O << '#';
      operand(I->getOperand(4), O);
    } else {
      int Mode = I->getOperand(S + 1).getImm();
      if (Mode == 0)
        O << '[';
      else if (Mode == 1)
        O << '(';
      operand(I->getOperand(S), O);
      if (Mode == 0)
        O << ']';
      else if (Mode == 1)
        O << ')';
    }
    printAnnotation(O, A);
  }
};
} // namespace
const char *llvm::Squall::aluName(unsigned Op) {
  return Op < 35 ? Names[Op] : nullptr;
}
int llvm::Squall::aluCode(StringRef N) {
  for (int I = 0; I < 35; ++I)
    if (Names[I] && N == Names[I])
      return I;
  return -1;
}
uint64_t llvm::Squall::encode(const MCInst &I, SmallVectorImpl<MCFixup> &F,
                              MCContext &C) {
  auto field = [&](unsigned N, unsigned Bits, bool Signed, unsigned Shift,
                   int Fix = -1) -> uint64_t {
    const auto &V = I.getOperand(N);
    if (V.isExpr() && Fix >= 0) {
      F.push_back(MCFixup::create(0, V.getExpr(), Fix, true));
      return 0;
    }
    if (!V.isImm()) {
      C.reportError(I.getLoc(), "expected an immediate operand");
      return 0;
    }
    int64_t X = V.getImm();
    if (Signed ? !isIntN(Bits, X) : !isUIntN(Bits, X))
      C.reportError(I.getLoc(), "Squall instruction field out of range");
    return (uint64_t(X) & ((UINT64_C(1) << Bits) - 1)) << Shift;
  };
  unsigned K = I.getOpcode();
  if (K == WRITE)
    return 2 | field(0, 29, false, 2);
  bool Alu = K == ALUI || K == ALUS;
  unsigned D = Alu ? 1 : 0, S = K == ALUS || K == EXTRACT ? 4 : 2;
  uint64_t W = field(D, 20, true, 43, Dest20) | field(D + 1, 1, false, 63);
  if (Alu) {
    if (!aluName(I.getOperand(0).getImm()))
      C.reportError(I.getLoc(), "unknown Squall ALU opcode");
    W |= field(0, 6, false, 2);
    if (I.getOperand(3).getImm() != -1)
      W |= UINT64_C(1) << 8 | field(3, 1, false, 9);
  }
  if (K == ALUI)
    return W | field(4, 32, true, 10);
  int64_t Mode = I.getOperand(S + 1).getImm(),
          Rule = I.getOperand(S + 2).getImm();
  if (Mode > 2 || Rule > 2)
    C.reportError(I.getLoc(), "reserved addressing mode or matching rule");
  W |= 1 | field(S, 11, Mode == 1, 28, Mode == 1 ? Code11 : -1) |
       field(S + 1, 2, false, 39) | field(S + 2, 2, false, 41);
  if (K == SEND)
    W |= UINT64_C(3) << 26;
  if (K == EXTRACT)
    W |= UINT64_C(2) << 26 | field(2, 20, true, 2, Target20) |
         field(3, 1, false, 22);
  return W;
}
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeSquallTargetMC() {
  Target &T = getTheSquallTarget();
  TargetRegistry::RegisterMCAsmInfo(
      T,
      [](const MCRegisterInfo &, const Triple &, const MCTargetOptions &O)
          -> MCAsmInfo * { return new SquallAsmInfo(O); });
  TargetRegistry::RegisterMCInstrInfo(T, []() -> MCInstrInfo * {
    auto *I = new MCInstrInfo();
    InitSquallMCInstrInfo(I);
    return I;
  });
  TargetRegistry::RegisterMCRegInfo(T, [](const Triple &) -> MCRegisterInfo * {
    auto *R = new MCRegisterInfo();
    InitSquallMCRegisterInfo(R, Squall::FP);
    return R;
  });
  TargetRegistry::RegisterMCSubtargetInfo(
      T,
      [](const Triple &TT, StringRef CPU, StringRef FS) -> MCSubtargetInfo * {
        return createSquallMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
      });
  TargetRegistry::RegisterMCInstPrinter(
      T,
      [](const Triple &, unsigned V, const MCAsmInfo &A, const MCInstrInfo &I,
         const MCRegisterInfo &R) -> MCInstPrinter * {
        return V == 0 ? new Printer(A, I, R) : nullptr;
      });
  TargetRegistry::RegisterMCCodeEmitter(
      T, [](const MCInstrInfo &, MCContext &C) -> MCCodeEmitter * {
        return new Emitter(C);
      });
  TargetRegistry::RegisterMCAsmBackend(T, createSquallAsmBackend);
}
