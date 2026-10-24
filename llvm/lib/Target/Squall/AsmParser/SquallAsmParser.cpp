// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
namespace {
class Parser : public MCTargetAsmParser {
  MCAsmParser &P;
  MCInst Inst;
  bool HasInstruction = false;
  void imm(int64_t X) { Inst.addOperand(MCOperand::createImm(X)); }
  bool end() { return P.parseEOL(); }
  bool integer(int64_t &V) { return P.parseAbsoluteExpression(V); }
  bool error(const Twine &S) { return P.Error(P.getTok().getLoc(), S); }
  bool destination(int &Secondary) {
    Secondary = -1;
    const MCExpr *E = nullptr;
    bool Literal = P.getTok().is(AsmToken::LParen);
    StringRef Name, Ports;
    if (Literal) {
      P.Lex();
      int64_t V;
      if (integer(V) ||
          P.parseToken(AsmToken::RParen, "expected ')' after word offset"))
        return true;
      E = MCConstantExpr::create(V, P.getContext());
      if (P.getTok().isNot(AsmToken::Identifier))
        return error("expected destination .l or .r port");
      Ports = P.getTok().getString();
    } else {
      if (P.getTok().isNot(AsmToken::Identifier))
        return error("expected instruction label or parenthesized word offset");
      StringRef Full = P.getTok().getString();
      size_t Dot = Full.rfind('.');
      if (Dot != StringRef::npos && Dot >= 2 && Full[Dot - 2] == '.')
        Dot -= 2;
      if (Dot == StringRef::npos)
        return error("destination must end in .l or .r");
      Name = Full.take_front(Dot);
      Ports = Full.drop_front(Dot);
      E = MCSymbolRefExpr::create(P.getContext().getOrCreateSymbol(Name),
                                  P.getContext(), P.getTok().getLoc());
    }
    if ((Ports.size() != 2 && Ports.size() != 4) || Ports[0] != '.' ||
        (Ports[1] != 'l' && Ports[1] != 'r') ||
        (Ports.size() == 4 &&
         (Ports[2] != '.' || (Ports[3] != 'l' && Ports[3] != 'r'))))
      return error(
          "expected .l or .r, optionally followed by a secondary port");
    P.Lex();
    if (Literal)
      imm(cast<MCConstantExpr>(E)->getValue());
    else
      Inst.addOperand(MCOperand::createExpr(E));
    imm(Ports[1] == 'r');
    if (Ports.size() == 4)
      Secondary = Ports[3] == 'r';
    if (P.getTok().is(AsmToken::Identifier) &&
        (P.getTok().getString() == "l" || P.getTok().getString() == "r")) {
      if (Secondary >= 0)
        return error("duplicate secondary port");
      Secondary = P.getTok().getString() == "r";
      P.Lex();
    }
    return false;
  }
  bool slot(int Rule) {
    int Mode = 2;
    const MCExpr *E = nullptr;
    if (P.getTok().is(AsmToken::LBrac)) {
      Mode = 0;
      P.Lex();
      int64_t V;
      if (integer(V) || P.parseToken(AsmToken::RBrac, "expected ']'"))
        return true;
      imm(V);
    } else if (P.getTok().is(AsmToken::LParen)) {
      Mode = 1;
      P.Lex();
      if (P.parseExpression(E) ||
          P.parseToken(AsmToken::RParen, "expected ')'"))
        return true;
      int64_t V;
      if (E->evaluateAsAbsolute(V))
        imm(V);
      else
        Inst.addOperand(MCOperand::createExpr(E));
    } else {
      int64_t V;
      if (integer(V))
        return true;
      imm(V);
    }
    imm(Mode);
    imm(Rule);
    return false;
  }

public:
  Parser(const MCSubtargetInfo &S, MCAsmParser &P, const MCInstrInfo &I)
      : MCTargetAsmParser(S, I), P(P) {
    // Raw .text is conventionally loaded at physical word 256. Image-producing
    // codegen overrides halt to zero because its section starts at physical 0.
    auto *H = P.getContext().getOrCreateSymbol("halt");
    H->setVariableValue(MCConstantExpr::create(-2048, P.getContext()));
  }
  bool parseRegister(MCRegister &, SMLoc &, SMLoc &) override {
    return error("Squall has no registers");
  }
  ParseStatus tryParseRegister(MCRegister &, SMLoc &, SMLoc &) override {
    return ParseStatus::NoMatch;
  }
  ParseStatus parseDirective(AsmToken T) override {
    StringRef N = T.getString();
    if (N != ".slot" && N != ".word" && N != ".squall_origin")
      return ParseStatus::NoMatch;
    int64_t V;
    if (integer(V))
      return ParseStatus::Failure;
    if (N == ".squall_origin") {
      if (HasInstruction) {
        error(".squall_origin must precede instructions");
        return ParseStatus::Failure;
      }
      if (!isUInt<29>(V)) {
        error("origin must fit an unsigned 29-bit word address");
        return ParseStatus::Failure;
      }
      if (end())
        return ParseStatus::Failure;
      P.getContext().getOrCreateSymbol("halt")->setVariableValue(
          MCConstantExpr::create(-V * 8, P.getContext()));
      return ParseStatus::Success;
    }
    if (N == ".slot") {
      if (!isInt<62>(V)) {
        error("constant slot payload must fit signed 62 bits");
        return ParseStatus::Failure;
      }
      V = (uint64_t(V) & ((UINT64_C(1) << 62) - 1)) | (UINT64_C(2) << 62);
    }
    if (end())
      return ParseStatus::Failure;
    P.getStreamer().emitIntValue(uint64_t(V), 8);
    return ParseStatus::Success;
  }
  bool parseInstruction(ParseInstructionInfo &, StringRef Name, SMLoc L,
                        OperandVector &) override {
    HasInstruction = true;
    Inst.clear();
    Inst.setLoc(L);
    auto Parts = Name.split('.');
    int Rule = 0;
    if (!Parts.second.empty()) {
      if (Parts.second != "r" && Parts.second != "c")
        return error("matching suffix must be .r or .c");
      Rule = Parts.second == "r" ? 2 : 1;
    }
    int Op = Squall::aluCode(Parts.first);
    if (Name == "write") {
      Inst.setOpcode(Squall::WRITE);
      int64_t V;
      if (integer(V))
        return true;
      imm(V);
      return end();
    }
    bool Send = Parts.first == "send", Extract = Parts.first == "extract";
    if (Op < 0 && !Send && !Extract)
      return error("unknown Squall instruction");
    if (Op >= 0)
      imm(Op);
    int Second;
    if (destination(Second))
      return true;
    if (Send || Extract) {
      if (Second >= 0)
        return error("send/extract cannot have a secondary ALU destination");
      Inst.setOpcode(Send ? Squall::SEND : Squall::EXTRACT);
      if (Extract) {
        int Other;
        if (destination(Other))
          return true;
        if (Other >= 0)
          return error("extract target cannot have a secondary port");
      }
    } else
      imm(Second);
    if (P.getTok().is(AsmToken::Hash)) {
      if (Op < 0 || !Parts.second.empty())
        return error(
            "immediates require an ALU instruction without a matching suffix");
      Inst.setOpcode(Squall::ALUI);
      P.Lex();
      int64_t V;
      if (integer(V))
        return true;
      imm(V);
    } else {
      if (Op >= 0)
        Inst.setOpcode(Squall::ALUS);
      if (slot(Rule))
        return true;
    }
    return end();
  }
  void convertToMapAndConstraints(unsigned, const OperandVector &) override {}
  bool matchAndEmitInstruction(SMLoc, unsigned &Opcode, OperandVector &,
                               MCStreamer &Out, uint64_t &, bool) override {
    SmallVector<MCFixup, 2> Fixups;
    Squall::encode(Inst, Fixups, P.getContext());
    if (P.getContext().hadError())
      return true;
    Opcode = Inst.getOpcode();
    Out.emitInstruction(Inst, getSTI());
    return false;
  }
};
} // namespace
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeSquallAsmParser() {
  RegisterMCAsmParser<Parser> X(getTheSquallTarget());
}
