// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
namespace {
class Decoder : public MCDisassembler {
public:
  using MCDisassembler::MCDisassembler;
  DecodeStatus getInstruction(MCInst &I, uint64_t &Size, ArrayRef<uint8_t> B,
                              uint64_t, raw_ostream &) const override {
    if (B.size() < 8) {
      Size = 0;
      return Fail;
    }
    Size = 8;
    uint64_t W = support::endian::read64le(B.data());
    auto imm = [&](int64_t V) { I.addOperand(MCOperand::createImm(V)); };
    auto bits = [&](unsigned Hi, unsigned Lo) {
      return (W >> Lo) & ((UINT64_C(1) << (Hi - Lo + 1)) - 1);
    };
    auto destination = [&]() {
      imm(SignExtend64<20>(bits(62, 43)));
      imm(bits(63, 63));
    };
    unsigned Form = W & 3;
    if (Form == 3)
      return Fail;
    if (Form == 2) {
      bool Ack = bits(39, 39);
      // Reserved bits, and a destination that is not acknowledged.
      if (bits(42, 40) || (!Ack && bits(63, 43)))
        return Fail;
      I.setOpcode(Squall::WRITE);
      imm(bits(30, 2));
      imm(bits(38, 31));
      destination();
      imm(Ack);
      return Success;
    }
    bool Other = bits(42, 42); // Extract (monadic) or Send (dyadic).
    if (Form == 0 && Other) {
      // Bits 41:31 are Extract's pad, which nothing reads; the upstream Zig
      // assembler does not always leave them zero, so they are ignored.
      if (bits(9, 2))
        return Fail;
      I.setOpcode(Squall::EXTRACT);
      destination();
      imm(SignExtend64<20>(bits(29, 10)));
      imm(bits(30, 30));
      return Success;
    }
    if (!Other) {
      if (!Squall::aluName(bits(7, 2)) || (bits(9, 9) && !bits(8, 8)))
        return Fail;
    } else if (bits(25, 2)) {
      return Fail;
    }
    unsigned Mode = bits(39, 38), Rule = bits(41, 40);
    if (Form == 1 && (Mode > 2 || Rule > 2 || (!Other && bits(25, 10))))
      return Fail;
    I.setOpcode(Form == 0 ? Squall::ALUI
                : Other   ? Squall::SEND
                          : Squall::ALUS);
    if (!Other)
      imm(bits(7, 2));
    destination();
    if (!Other)
      imm(bits(8, 8) ? bits(9, 9) : -1);
    if (Form == 0) {
      imm(SignExtend64<32>(bits(41, 10)));
      return Success;
    }
    imm(Mode == 1 ? SignExtend64<12>(bits(37, 26)) : bits(37, 26));
    imm(Mode);
    imm(Rule);
    return Success;
  }
};
} // namespace
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeSquallDisassembler() {
  TargetRegistry::RegisterMCDisassembler(
      getTheSquallTarget(),
      [](const Target &, const MCSubtargetInfo &S,
         MCContext &C) -> MCDisassembler * { return new Decoder(S, C); });
}
