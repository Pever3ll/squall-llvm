//===-- SquallAsmParser.cpp - Parse Squall assembly to MCInst -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/SquallMCExpr.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "squall-asm-parser"

namespace {

//===----------------------------------------------------------------------===//
// SquallOperand
//===----------------------------------------------------------------------===//

/// A parsed Squall assembly operand.
///
/// Four kinds:
///   k_Token  - the mnemonic
///   k_Imm    - port, second_port, simm32, ptr (one MCOperand)
///   k_IDest  - d.off / d2.off, carrying the parenthesised flag
///   k_FDest  - addr + addr_tag (two MCOperands)
class SquallOperand : public MCParsedAsmOperand {
public:
  enum KindTy {
    k_Token,
    k_Imm,
    k_IDest,
    k_FDest,
  };

private:
  KindTy Kind;
  SMLoc StartLoc, EndLoc;

  struct TokOp {
    const char *Data;
    unsigned Length;
  };
  struct ImmOp {
    const MCExpr *Val;
  };
  struct IDestOp {
    const MCExpr *Val;
    bool Parenthesised;
  };
  struct FDestOp {
    const MCExpr *Addr;
    unsigned At; // 0 frame (unsigned), 1 code (signed), 2 absolute (unsigned)
  };

  union {
    struct TokOp Tok;
    struct ImmOp Imm;
    struct IDestOp IDest;
    struct FDestOp FDest;
  };

public:
  SquallOperand(KindTy K) : Kind(K) {}

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  KindTy getKind() const { return Kind; }

  //--- MCParsedAsmOperand interface --------------------------------------

  bool isToken() const override { return Kind == k_Token; }
  bool isImm() const override { return Kind == k_Imm; }
  bool isReg() const override { return false; }
  bool isMem() const override { return false; }

  MCRegister getReg() const override {
    llvm_unreachable("Squall has no registers");
  }

  StringRef getToken() const {
    assert(Kind == k_Token && "not a token");
    return StringRef(Tok.Data, Tok.Length);
  }

  const MCExpr *getImm() const {
    assert(Kind == k_Imm && "not an immediate");
    return Imm.Val;
  }

  //--- Range helpers -----------------------------------------------------
  //
  // Symbols return true: they cannot be checked until fixup application.

  bool isImmInRange(int64_t Lo, int64_t Hi) const {
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    if (!CE)
      return true;
    int64_t V = CE->getValue();
    return V >= Lo && V <= Hi;
  }

  bool isImmUIntN(unsigned N) const {
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    if (!CE)
      return true;
    return isUIntN(N, CE->getValue());
  }

  bool isImmIntN(unsigned N) const {
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    if (!CE)
      return true;
    return isIntN(N, CE->getValue());
  }

  //--- Predicates named after AsmOperandClass::Name ----------------------
  //
  // AsmMatcher generates calls to is<Name>() for each ParserMatchClass.

  bool isPort() const { return Kind == k_Imm && isImmInRange(0, 1); }
  bool isSecondPort() const { return Kind == k_Imm && isImmInRange(0, 3); }
  bool isPtr() const { return Kind == k_Imm && isImmUIntN(29); }
  bool isSImm32() const { return Kind == k_Imm && isImmIntN(32); }
  bool isIDest() const { return Kind == k_IDest; }
  bool isFDest() const { return Kind == k_FDest; }

  //--- Render methods named in AsmOperandClass::RenderMethod -------------

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "imm renders as one operand");
    if (const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Imm.Val));
  }

  void addIDestOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "idest renders as one operand");
    if (const auto *CE = dyn_cast<MCConstantExpr>(IDest.Val))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(IDest.Val));
  }

  void addFDestOperands(MCInst &Inst, unsigned N) const {
    assert(N == 2 && "fdest renders as {addr, at}");
    if (const auto *CE = dyn_cast<MCConstantExpr>(FDest.Addr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(FDest.Addr));
    Inst.addOperand(MCOperand::createImm(FDest.At));
  }

  //--- Factories ---------------------------------------------------------

  static std::unique_ptr<SquallOperand> createToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<SquallOperand>(k_Token);
    Op->Tok.Data = Str.data();
    Op->Tok.Length = Str.size();
    Op->StartLoc = S;
    Op->EndLoc = S;
    return Op;
  }

  static std::unique_ptr<SquallOperand> createImm(const MCExpr *Val, SMLoc S,
                                                  SMLoc E) {
    auto Op = std::make_unique<SquallOperand>(k_Imm);
    Op->Imm.Val = Val;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<SquallOperand>
  createIDest(const MCExpr *Val, bool Paren, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<SquallOperand>(k_IDest);
    Op->IDest.Val = Val;
    Op->IDest.Parenthesised = Paren;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<SquallOperand>
  createFDest(const MCExpr *Addr, unsigned At, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<SquallOperand>(k_FDest);
    Op->FDest.Addr = Addr;
    Op->FDest.At = At;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  void print(raw_ostream &OS) const override {
    switch (Kind) {
    case k_Token:
      OS << "Token:" << getToken();
      break;
    case k_Imm:
      OS << "Imm:" << *Imm.Val;
      break;
    case k_IDest:
      OS << "IDest:" << *IDest.Val
         << (IDest.Parenthesised ? " (paren)" : " (bare)");
      break;
    case k_FDest:
      OS << "FDest:" << *FDest.Addr << " at=" << FDest.At;
      break;
    }
  }
};

//===----------------------------------------------------------------------===//
// SquallAsmParser
//===----------------------------------------------------------------------===//

class SquallAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }
  SMLoc getLoc() const { return getParser().getTok().getLoc(); }

  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);

#define GET_ASSEMBLER_HEADER
#include "SquallGenAsmMatcher.inc"

public:
  SquallAsmParser(const MCSubtargetInfo &STI, MCAsmParser &P,
                  const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(P) {
    MCAsmParserExtension::Initialize(P);
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }

  //--- MCTargetAsmParser interface ---------------------------------------

  bool ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  bool MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override {
    return true; // Squall exposes no registers.
  }

  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override {
    return ParseStatus::NoMatch;
  }

  bool ParseDirective(AsmToken DirectiveID) override { return true; }

  //--- Custom ParserMethods ----------------------------------------------

  ParseStatus parsePort(OperandVector &Operands);
  ParseStatus parseSecondPort(OperandVector &Operands);
  ParseStatus parseIDest(OperandVector &Operands);
  ParseStatus parseFDest(OperandVector &Operands);
  ParseStatus parsePtr(OperandVector &Operands);
  ParseStatus parseSImm32(OperandVector &Operands);

  //--- DefaultMethod for second_port's IsOptional ------------------------

  std::unique_ptr<SquallOperand> defaultSecondPort() const;
};

} // end anonymous namespace

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "SquallGenAsmMatcher.inc"

//===----------------------------------------------------------------------===//
// Custom operand parsers
//===----------------------------------------------------------------------===//

/// port: the mandatory '.l' / '.r' suffix on a destination.
///
/// The lexer may deliver this as Dot + Identifier, or as a single Identifier
/// beginning with '.', depending on what preceded it. Handle both.
ParseStatus SquallAsmParser::parsePort(OperandVector &Operands) {
  SMLoc S = getLoc();
  AsmLexer &Lex = getLexer();
  StringRef Letter;
  SMLoc E;

  if (Lex.is(AsmToken::Dot)) {
    Lex.Lex(); // eat '.'
    if (Lex.isNot(AsmToken::Identifier))
      return Error(getLoc(), "expected 'l' or 'r' after '.'");
    Letter = Lex.getTok().getIdentifier();
    E = Lex.getTok().getEndLoc();
    Lex.Lex();
  } else if (Lex.is(AsmToken::Identifier) &&
             Lex.getTok().getIdentifier().starts_with(".")) {
    Letter = Lex.getTok().getIdentifier().drop_front();
    E = Lex.getTok().getEndLoc();
    Lex.Lex();
  } else {
    return ParseStatus::NoMatch;
  }

  unsigned Val;
  if (Letter == "l")
    Val = 0;
  else if (Letter == "r")
    Val = 1;
  else
    return Error(S, "invalid port '" + Letter + "', expected 'l' or 'r'");

  Operands.push_back(SquallOperand::createImm(
      MCConstantExpr::create(Val, getContext()), S, E));
  return ParseStatus::Success;
}

/// second_port: the optional trailing 'l' / 'r'.
///
/// Encodes {s, p2} as a single value: 0 absent, 2 'l', 3 'r'.
///
/// Returns NoMatch rather than an error when the token is not a port, since
/// a bare identifier here could instead begin an absolute fdest. One
/// consequence: symbols literally named 'l' or 'r' cannot be used as an
/// absolute fdest, because this parser claims them first.
ParseStatus SquallAsmParser::parseSecondPort(OperandVector &Operands) {
  AsmLexer &Lex = getLexer();

  if (Lex.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  StringRef Tok = Lex.getTok().getIdentifier();
  unsigned Val;
  if (Tok == "l")
    Val = 2; // s=1, p2=0
  else if (Tok == "r")
    Val = 3; // s=1, p2=1
  else
    return ParseStatus::NoMatch;

  SMLoc S = getLoc(), E = Lex.getTok().getEndLoc();
  Lex.Lex();

  Operands.push_back(SquallOperand::createImm(
      MCConstantExpr::create(Val, getContext()), S, E));
  return ParseStatus::Success;
}

std::unique_ptr<SquallOperand> SquallAsmParser::defaultSecondPort() const {
  return SquallOperand::createImm(MCConstantExpr::create(0, getContext()),
                                  SMLoc(), SMLoc());
}

/// idest / ipayload: '(expr)' or 'expr'.
///
/// Both spellings land in the same 20-bit signed word-offset field. The
/// parens mark the operand as an already-relative displacement; a bare
/// symbol must be relativised by the linker. That distinction is carried to
/// the encoder by wrapping the expression in a SquallMCExpr.
ParseStatus SquallAsmParser::parseIDest(OperandVector &Operands) {
  SMLoc S = getLoc();
  AsmLexer &Lex = getLexer();
  bool Paren = false;

  if (Lex.is(AsmToken::LParen)) {
    Paren = true;
    Lex.Lex();
  }

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  if (Paren) {
    if (Lex.isNot(AsmToken::RParen))
      return Error(getLoc(), "expected ')' after destination offset");
    Lex.Lex();
  }

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
    if (!isInt<20>(CE->getValue()))
      return Error(S, "destination offset out of range "
                      "(must be -524288..524287)");
  } else if (Paren) {
    Expr =
        SquallMCExpr::create(Expr, SquallMCExpr::VK_Squall_REL, getContext());
  }

  Operands.push_back(SquallOperand::createIDest(Expr, Paren, S, getLoc()));
  return ParseStatus::Success;
}

/// fdest: '[expr]' (frame, unsigned), '(expr)' (code, signed), or 'expr'
/// (absolute, unsigned). The bracket form selects addr_tag, and addr_tag
/// determines addr's signedness.
ParseStatus SquallAsmParser::parseFDest(OperandVector &Operands) {
  SMLoc S = getLoc();
  AsmLexer &Lex = getLexer();

  unsigned At;
  AsmToken::TokenKind Closer = AsmToken::Eof; // Eof = no closer expected

  if (Lex.is(AsmToken::LBrac)) {
    At = 0; // frame-relative, unsigned
    Closer = AsmToken::RBrac;
    Lex.Lex();
  } else if (Lex.is(AsmToken::LParen)) {
    At = 1; // code-relative, signed
    Closer = AsmToken::RParen;
    Lex.Lex();
  } else {
    At = 2; // absolute, unsigned
  }

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  if (Closer != AsmToken::Eof) {
    if (Lex.isNot(Closer))
      return Error(getLoc(), At == 0 ? "expected ']'" : "expected ')'");
    Lex.Lex();
  }

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
    int64_t V = CE->getValue();
    if (At == 1) {
      if (!isInt<11>(V))
        return Error(S, "code-relative slot offset out of range "
                        "(must be -1024..1023)");
    } else if (!isUInt<11>(V)) {
      return Error(S, "slot address out of range (must be 0..2047)");
    }
  }

  Operands.push_back(SquallOperand::createFDest(Expr, At, S, getLoc()));
  return ParseStatus::Success;
}

/// ptr: Write target, a 29-bit unsigned absolute word index.
ParseStatus SquallAsmParser::parsePtr(OperandVector &Operands) {
  SMLoc S = getLoc();

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
    if (!isUInt<29>(CE->getValue()))
      return Error(S, "word index out of range (must be 0..536870911)");

  Operands.push_back(SquallOperand::createImm(Expr, S, getLoc()));
  return ParseStatus::Success;
}

/// simm32: the monadic immediate, written '#expr'.
///
/// The '#' sigil is what disambiguates a monadic instruction from a dyadic
/// one with an absolute fdest, since both otherwise end in a bare integer.
ParseStatus SquallAsmParser::parseSImm32(OperandVector &Operands) {
  SMLoc S = getLoc();
  AsmLexer &Lex = getLexer();

  if (Lex.isNot(AsmToken::Hash))
    return ParseStatus::NoMatch;
  Lex.Lex(); // eat '#'

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
    if (!isInt<32>(CE->getValue()) && !isUInt<32>(CE->getValue()))
      return Error(S, "immediate out of range (must fit in 32 bits)");

  Operands.push_back(SquallOperand::createImm(Expr, S, getLoc()));
  return ParseStatus::Success;
}

//===----------------------------------------------------------------------===//
// Instruction parsing
//===----------------------------------------------------------------------===//

bool SquallAsmParser::parseOperand(OperandVector &Operands,
                                   StringRef Mnemonic) {
  // Dispatch to the ParserMethod declared for this operand position in the
  // .td file. Generated from the AsmOperandClass ParserMethod fields.
  ParseStatus Res = MatchOperandParserImpl(Operands, Mnemonic,
                                           /*ParseForAllFeatures=*/true);
  if (Res.isSuccess())
    return false;
  if (Res.isFailure())
    return true;

  // No ParserMethod claimed the token. Fall back to a plain expression so
  // that operands without a custom class still work.
  SMLoc S = getLoc();
  const MCExpr *Expr;
  if (!getParser().parseExpression(Expr)) {
    Operands.push_back(SquallOperand::createImm(Expr, S, getLoc()));
    return false;
  }

  return Error(getLoc(), "unexpected token in operand list");
}

bool SquallAsmParser::ParseInstruction(ParseInstructionInfo &Info,
                                       StringRef Name, SMLoc NameLoc,
                                       OperandVector &Operands) {
  // The matching-rule suffix (.r / .c) is folded into the mnemonic by the
  // ALUOp / ExtractOp / SendOp multiclasses, so Name already distinguishes
  // "add", "add.r" and "add.c". No splitting required here.
  Operands.push_back(SquallOperand::createToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement)) {
    getParser().Lex();
    return false;
  }

  // Squall separates operands by whitespace, not commas, so this is a simple
  // loop with no separator handling. The AsmLexer has already collapsed runs of
  // whitespace, so adjacent operands are simply adjacent tokens.
  while (getLexer().isNot(AsmToken::EndOfStatement)) {
    if (parseOperand(Operands, Name))
      return true;
  }

  getParser().Lex(); // eat EndOfStatement
  return false;
}

bool SquallAsmParser::MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                              OperandVector &Operands,
                                              MCStreamer &Out,
                                              uint64_t &ErrorInfo,
                                              bool MatchingInlineAsm) {
  MCInst Inst;
  FeatureBitset MissingFeatures;

  unsigned Result = MatchInstructionImpl(Operands, Inst, ErrorInfo,
                                         MissingFeatures, MatchingInlineAsm);

  switch (Result) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;

  case Match_MissingFeature:
    return Error(IDLoc, "instruction not supported on this subtarget");

  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");

  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL) {
      if (ErrorInfo >= Operands.size())
        return Error(IDLoc, "too few operands for instruction");
      ErrorLoc = ((SquallOperand &)*Operands[ErrorInfo]).getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  }

  // Operand-class-specific diagnostics generated from DiagnosticType /
  // DiagnosticString in the .td file land here.
  if (Result > FIRST_TARGET_MATCH_RESULT_TY) {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL && ErrorInfo < Operands.size())
      ErrorLoc = ((SquallOperand &)*Operands[ErrorInfo]).getStartLoc();

    switch (Result) {
    case Match_InvalidPort:
      return Error(ErrorLoc, "expected port '.l' or '.r'");
    case Match_InvalidSecondPort:
      return Error(ErrorLoc, "expected second port 'l' or 'r'");
    case Match_InvalidIDest:
      return Error(ErrorLoc, "expected destination offset or '(label)'");
    case Match_InvalidFDest:
      return Error(ErrorLoc, "expected '[frame]', '(code)' or an absolute "
                             "slot address");
    case Match_InvalidPtr:
      return Error(ErrorLoc, "expected absolute word index (29-bit unsigned)");
    case Match_InvalidSImm32:
      return Error(ErrorLoc, "expected 32-bit immediate '#value'");
    default:
      break;
    }
  }

  llvm_unreachable("unhandled match result");
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeSquallAsmParser() {
  RegisterMCAsmParser<SquallAsmParser> X(getTheSquallTarget());
}
