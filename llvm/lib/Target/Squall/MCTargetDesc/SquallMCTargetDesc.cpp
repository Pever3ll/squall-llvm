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
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
#define GET_INSTRINFO_MC_DESC
#include "SquallGenInstrInfo.inc"
#define GET_REGINFO_MC_DESC
#include "SquallGenRegisterInfo.inc"
#define GET_SUBTARGETINFO_MC_DESC
#include "SquallGenSubtargetInfo.inc"
namespace {
constexpr unsigned NumAluOps = 37;
const char *Names[NumAluOps] = {
    "nop",  nullptr, nullptr, nullptr, "add",   "sub",   "mul",   "div", "divu",
    "rem",  "mod",   nullptr, nullptr, nullptr, nullptr, nullptr, "and", "or",
    "xor",  "shl",   "shrl",  "shra",  nullptr, nullptr, "not",   "rev", "dupl",
    "dupr", nullptr, nullptr, nullptr, nullptr, "beqz",  "beql",  "beqr",
    "bler", "blel"};
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
      // write[.a ack-dest] slot [byte-mask]
      bool Ack = I->getOperand(4).getImm();
      if (Ack) {
        O << ".a ";
        dest(I, 2, O);
      }
      O << ' ';
      operand(I->getOperand(0), O);
      int64_t Mask = I->getOperand(1).getImm();
      if (Mask != 0xff)
        O << ' ' << format_hex(uint64_t(Mask) & 0xff, 4);
      printAnnotation(O, A);
      return;
    }
    if (K == Squall::EXTRACT) {
      O << ' ';
      dest(I, 0, O);
      O << ' ';
      dest(I, 2, O);
      printAnnotation(O, A);
      return;
    }
    unsigned D = K == Squall::ALUI || K == Squall::ALUS ? 1 : 0;
    unsigned S = K == Squall::ALUS ? 4 : 2;
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
  return Op < NumAluOps ? Names[Op] : nullptr;
}
int llvm::Squall::aluCode(StringRef N) {
  for (unsigned I = 0; I < NumAluOps; ++I)
    if (Names[I] && N == Names[I])
      return I;
  return -1;
}
// Field layout of the Squall v2 ISA as implemented by Isacomp's
// squall-v2-rebase RTL (isa/squall/{fetch,dest,slot,alu_operands}.isa and
// instr/{send,extract,write}.isa):
//
//   all       [1:0] form: 0 monadic, 1 dyadic, 2 write
//             [62:43] signed destination offset, [63] destination port
//   monadic   [7:2] alu op, [8] second token, [9] its port, [41:10] literal,
//             [42] forming rule: 0 Arith, 1 Extract
//   extract   [29:10] signed second-destination offset, [30] its port
//   dyadic    [7:2] alu op, [8] second token, [9] its port (Arith only),
//             [37:26] slot, [39:38] mode, [41:40] matching rule,
//             [42] forming rule: 0 Arith, 1 Send
//   write     [30:2] word, [38:31] byte mask, [39] acknowledge
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
  auto destination = [&](unsigned N) {
    return field(N, 20, true, 43, Dest20) | field(N + 1, 1, false, 63);
  };
  unsigned K = I.getOpcode();
  if (K == WRITE) {
    uint64_t W = 2 | field(0, 29, false, 2) | field(1, 8, false, 31) |
                 field(4, 1, false, 39);
    if (I.getOperand(4).isImm() && I.getOperand(4).getImm())
      W |= destination(2);
    return W;
  }
  uint64_t W = destination(K == ALUI || K == ALUS ? 1 : 0);
  if (K == EXTRACT)
    return W | UINT64_C(1) << 42 | field(2, 20, true, 10, Target20) |
           field(3, 1, false, 30);
  if (K == ALUI || K == ALUS) {
    if (!aluName(I.getOperand(0).getImm()))
      C.reportError(I.getLoc(), "unknown Squall ALU opcode");
    W |= field(0, 6, false, 2);
    if (I.getOperand(3).getImm() != -1)
      W |= UINT64_C(1) << 8 | field(3, 1, false, 9);
  }
  if (K == ALUI)
    return W | field(4, 32, true, 10);
  unsigned S = K == ALUS ? 4 : 2;
  int64_t Mode = I.getOperand(S + 1).getImm(),
          Rule = I.getOperand(S + 2).getImm();
  if (Mode > 2 || Rule > 2)
    C.reportError(I.getLoc(), "reserved addressing mode or matching rule");
  W |= 1 | field(S, 12, Mode == 1, 26, Mode == 1 ? Code12 : -1) |
       field(S + 1, 2, false, 38) | field(S + 2, 2, false, 40);
  if (K == SEND)
    W |= UINT64_C(1) << 42;
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
