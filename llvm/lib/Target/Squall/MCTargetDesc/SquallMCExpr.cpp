// MCTargetDesc/SquallMCExpr.cpp
#include "SquallMCExpr.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCValue.h"

using namespace llvm;

#define DEBUG_TYPE "squall-mcexpr"

const SquallMCExpr *SquallMCExpr::create(const MCExpr *Expr, VariantKind Kind,
                                         MCContext &Ctx) {
  return new (Ctx) SquallMCExpr(Expr, Kind);
}

void SquallMCExpr::printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const {
  OS << '(';
  Expr->print(OS, MAI);
  OS << ')';
}

bool SquallMCExpr::evaluateAsRelocatableImpl(MCValue &Res,
                                             const MCAssembler *Asm,
                                             const MCFixup *Fixup) const {
  if (!getSubExpr()->evaluateAsRelocatable(Res, Asm, Fixup))
    return false;
  // The relative-vs-absolute choice is consumed by the code emitter when it
  // picks a fixup kind; nothing to fold here.
  return true;
}

void SquallMCExpr::visitUsedExpr(MCStreamer &Streamer) const {
  Streamer.visitUsedExpr(*getSubExpr());
}
