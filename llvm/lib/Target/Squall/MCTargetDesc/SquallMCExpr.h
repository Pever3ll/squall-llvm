// MCTargetDesc/SquallMCExpr.h
#ifndef LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCEXPR_H
#define LLVM_LIB_TARGET_SQUALL_MCTARGETDESC_SQUALLMCEXPR_H

#include "llvm/MC/MCExpr.h"

namespace llvm {

class SquallMCExpr : public MCTargetExpr {
public:
  enum VariantKind {
    VK_Squall_None,
    VK_Squall_REL, // operand was written '(sym)': relative displacement
  };

private:
  const MCExpr *Expr;
  const VariantKind Kind;

  explicit SquallMCExpr(const MCExpr *Expr, VariantKind Kind)
      : Expr(Expr), Kind(Kind) {}

public:
  static const SquallMCExpr *create(const MCExpr *Expr, VariantKind Kind,
                                    MCContext &Ctx);

  VariantKind getKind() const { return Kind; }
  const MCExpr *getSubExpr() const { return Expr; }

  void printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const override;
  bool evaluateAsRelocatableImpl(MCValue &Res, const MCAssembler *Asm,
                                 const MCFixup *Fixup) const override;
  void visitUsedExpr(MCStreamer &Streamer) const override;
  MCFragment *findAssociatedFragment() const override {
    return getSubExpr()->findAssociatedFragment();
  }
  void fixELFSymbolsInTLSFixups(MCAssembler &Asm) const override {}

  static bool classof(const MCExpr *E) {
    return E->getKind() == MCExpr::Target;
  }
};

} // namespace llvm
#endif
