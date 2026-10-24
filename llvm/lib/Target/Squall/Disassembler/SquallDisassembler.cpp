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
    if ((W & 3) == 2) {
      if (W >> 31)
        return Fail;
      I.setOpcode(Squall::WRITE);
      imm(W >> 2);
      return Success;
    }
    if ((W & 3) > 1)
      return Fail;
    bool Slot = W & 1;
    unsigned Kind = Slot ? (W >> 26) & 3 : 0;
    if (Kind == 1 || (Slot && (((W >> 39) & 3) > 2 || ((W >> 41) & 3) > 2)))
      return Fail;
    bool Alu = Kind == 0;
    if (Alu && (W & (1 << 9)) && !(W & (1 << 8)))
      return Fail;
    if (Alu && !Squall::aluName((W >> 2) & 63))
      return Fail;
    if ((!Slot && (W & (UINT64_C(1) << 42))) || (!Slot && (W & 2)))
      return Fail;
    I.setOpcode(!Slot       ? Squall::ALUI
                : Kind == 0 ? Squall::ALUS
                : Kind == 2 ? Squall::EXTRACT
                            : Squall::SEND);
    if (Alu)
      imm((W >> 2) & 63);
    imm(SignExtend64<20>((W >> 43) & 0xfffff));
    imm(W >> 63);
    if (Alu)
      imm(W & (1 << 8) ? (W >> 9) & 1 : -1);
    if (!Slot) {
      imm(SignExtend64<32>((W >> 10) & 0xffffffff));
      return Success;
    }
    if (Kind == 2) {
      imm(SignExtend64<20>((W >> 2) & 0xfffff));
      imm((W >> 22) & 1);
    }
    if ((Alu && (W & 0xffffc00)) || (Kind == 3 && (W & 0x3fffffc)) ||
        (Kind == 2 && (W & 0x3800000)))
      return Fail;
    unsigned Mode = (W >> 39) & 3;
    imm(Mode == 1 ? SignExtend64<11>((W >> 28) & 2047) : (W >> 28) & 2047);
    imm(Mode);
    imm((W >> 41) & 3);
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
