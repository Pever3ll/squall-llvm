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
  int64_t Origin = 256; // Word address of the start of .text.
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
      // The ports are the final ".l"/".r", preceded by a secondary ".l"/".r"
      // only when that component is a port: "loop.x.l" is label "loop.x".
      // "a.l.r" reads as label "a" with ports ".l.r", not label "a.l".
      size_t Dot = Full.rfind('.');
      if (Dot != StringRef::npos && Dot >= 2 && Full[Dot - 2] == '.' &&
          (Full[Dot - 1] == 'l' || Full[Dot - 1] == 'r'))
        Dot -= 2;
      if (Dot == StringRef::npos)
        return error("destination must end in .l or .r");
      if (Dot == 0)
        return error("expected a label before the destination port");
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
      : MCTargetAsmParser(S, I), P(P) {}
  /// `halt` names physical word 0, the discard destination. Raw .text is
  /// conventionally loaded at physical word 256 (see .squall_origin). The name
  /// is defined at the end of the file, and only when the file does not
  /// define it: compiled C may have a global or function called `halt`, and
  /// its generated code discards through `__squall_halt` instead.
  void onEndOfFile() override {
    MCSymbol *H = P.getContext().lookupSymbol("halt");
    if (H && H->isUndefined() && !H->isVariable())
      H->setVariableValue(MCConstantExpr::create(-Origin * 8, P.getContext()));
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
      Origin = V;
      return ParseStatus::Success;
    }
    if (N == ".slot") {
      // A slot read zero-extends its 62-bit payload, so a negative value
      // would not read back as itself.
      if (!isUInt<62>(V)) {
        error("constant slot payload must fit unsigned 62 bits");
        return ParseStatus::Failure;
      }
      V = uint64_t(V) | (UINT64_C(2) << 62);
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
    if (Parts.first == "write") {
      // write[.a ack-dest] word [byte-mask]
      if (!Parts.second.empty() && Parts.second != "a")
        return error("write suffix must be .a");
      bool Ack = Parts.second == "a";
      Inst.setOpcode(Squall::WRITE);
      MCInst Dest;
      if (Ack) {
        int Second;
        if (destination(Second))
          return true;
        if (Second >= 0)
          return error("write acknowledgement cannot have a secondary port");
        Dest = Inst; // The destination operands, parsed first.
        Inst.clear();
        Inst.setOpcode(Squall::WRITE);
        Inst.setLoc(L);
      }
      int64_t V, Mask = 0xff;
      if (integer(V))
        return true;
      imm(V);
      if (P.getTok().isNot(AsmToken::EndOfStatement) && integer(Mask))
        return true;
      imm(Mask);
      if (Ack) {
        Inst.addOperand(Dest.getOperand(0));
        Inst.addOperand(Dest.getOperand(1));
      } else {
        imm(0);
        imm(0);
      }
      imm(Ack);
      return end();
    }
    if (Parts.first == "extract") {
      if (!Parts.second.empty())
        return error("extract matches no slot and takes no matching suffix");
      Inst.setOpcode(Squall::EXTRACT);
      int Second, Other;
      if (destination(Second))
        return true;
      if (Second >= 0)
        return error("send/extract cannot have a secondary ALU destination");
      if (destination(Other))
        return true;
      if (Other >= 0)
        return error("extract target cannot have a secondary port");
      return end();
    }
    int Rule = 0;
    if (!Parts.second.empty()) {
      if (Parts.second != "r" && Parts.second != "c")
        return error("matching suffix must be .r or .c");
      Rule = Parts.second == "r" ? 2 : 1;
    }
    int Op = Squall::aluCode(Parts.first);
    bool Send = Parts.first == "send";
    if (Op < 0 && !Send)
      return error("unknown Squall instruction");
    if (Op >= 0)
      imm(Op);
    int Second;
    if (destination(Second))
      return true;
    if (Send) {
      if (Second >= 0)
        return error("send/extract cannot have a secondary ALU destination");
      Inst.setOpcode(Squall::SEND);
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
