//===-- SquallISelLowering.cpp - Squall DAG lowering --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Values are i32; wider integers are legalized into i32 pieces. Booleans are
// 0/1 in an i32 register. Squall's arithmetic is 64-bit, so the AsmPrinter
// keeps every register sign-extended from 32 bits.
//
// Memory is byte-addressed in four-byte little-endian cells, one per 64-bit
// slot. A cell holds its 32-bit value sign-extended, so i32 and pointer
// accesses are single slot accesses. Narrower accesses load the cell and
// extract the bytes; narrower stores rewrite the cell. Expanded code runs with
// one control token, so the read-modify-write cannot race.
//
// Calling convention: argument I is written to the callee's S<I>; the return
// value is the token value delivered to the caller's continuation. Calls
// preserve every register because each activation has a private frame. A
// byval argument is copied into a caller frame object, whose address is passed.
// A function pointer is the callee's entry word address, which is also a call
// tag with frame pointer 0, so indirect calls use the same convention.
//
//===----------------------------------------------------------------------===//

#include "SquallISelLowering.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallMachineFunctionInfo.h"
#include "SquallRegisterInfo.h"
#include "SquallSelectionDAGInfo.h"
#include "SquallSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

using namespace llvm;

SquallTargetLowering::SquallTargetLowering(const TargetMachine &TM,
                                           const SquallSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &Squall::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setBooleanContents(ZeroOrOneBooleanContent);
  setSchedulingPreference(Sched::Source);
  setMinFunctionAlignment(Align(8));
  setPrefFunctionAlignment(Align(8));
  setMinimumJumpTableEntries(UINT_MAX);
  setMaxAtomicSizeInBitsSupported(0);

  // There is no runtime library. Small memory intrinsics are expanded inline
  // here; larger and variable-length ones become loops before instruction
  // selection, as do 64-bit division and remainder.
  MaxStoresPerMemcpy = MaxStoresPerMemcpyOptSize = 256;
  MaxStoresPerMemmove = MaxStoresPerMemmoveOptSize = 256;
  MaxStoresPerMemset = MaxStoresPerMemsetOptSize = 256;
  setMaxDivRemBitWidthSupported(32);

  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::TRAP, MVT::Other, Legal);
  // Variadic arguments are in the argument registers, which are frame words;
  // va_list points at the first unnamed one. Arguments are packed four-byte
  // cells, so va_arg must not round the pointer up for eight-byte types.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  for (unsigned Op : {ISD::VAARG, ISD::VACOPY, ISD::VAEND})
    setOperationAction(Op, MVT::Other, Expand);
  setMinStackArgumentAlignment(Align(16));
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Expand);
  for (unsigned Op : {ISD::BR_CC,        ISD::BR_JT,
                      ISD::BRIND,        ISD::SELECT_CC,
                      ISD::ROTL,         ISD::ROTR,
                      ISD::BSWAP,        ISD::BITREVERSE,
                      ISD::CTPOP,        ISD::CTLZ,
                      ISD::CTTZ,         ISD::SMUL_LOHI,
                      ISD::UMUL_LOHI,    ISD::SDIVREM,
                      ISD::UDIVREM,      ISD::ABS,
                      ISD::SMIN,         ISD::SMAX,
                      ISD::UMIN,         ISD::UMAX,
                      ISD::SHL_PARTS,    ISD::SRA_PARTS,
                      ISD::SRL_PARTS,    ISD::FSHL,
                      ISD::FSHR,         ISD::UADDO,
                      ISD::USUBO,        ISD::SADDO,
                      ISD::SSUBO,        ISD::UMULO,
                      ISD::SMULO,        ISD::UADDSAT,
                      ISD::USUBSAT,      ISD::SADDSAT,
                      ISD::SSUBSAT,      ISD::STACKSAVE,
                      ISD::STACKRESTORE, ISD::DYNAMIC_STACKALLOC,
                      ISD::SETCCCARRY,   ISD::UADDO_CARRY,
                      ISD::USUBO_CARRY})
    setOperationAction(Op, MVT::i32, Expand);
  for (MVT VT : {MVT::i1, MVT::i8, MVT::i16})
    setOperationAction(ISD::SIGN_EXTEND_INREG, VT, Expand);
  for (MVT VT : {MVT::i8, MVT::i16}) {
    for (unsigned Ext : {ISD::EXTLOAD, ISD::ZEXTLOAD, ISD::SEXTLOAD})
      setLoadExtAction(Ext, MVT::i32, VT, Custom);
    setTruncStoreAction(MVT::i32, VT, Custom);
  }
  for (unsigned Ext : {ISD::EXTLOAD, ISD::ZEXTLOAD, ISD::SEXTLOAD})
    setLoadExtAction(Ext, MVT::i32, MVT::i1, Promote);
  setTruncStoreAction(MVT::i32, MVT::i1, Expand);
}

static void unsupported(SelectionDAG &DAG, const SDLoc &DL, const Twine &What) {
  DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
      DAG.getMachineFunction().getFunction(), What, DL.getDebugLoc()));
}

SDValue SquallTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:
    return lowerGlobalAddress(Op, DAG);
  case ISD::LOAD:
    return lowerNarrowLoad(Op, DAG);
  case ISD::STORE:
    return lowerNarrowStore(Op, DAG);
  case ISD::VASTART: {
    // The address of the first unnamed argument's register word.
    SDLoc DL(Op);
    MachineFunction &MF = DAG.getMachineFunction();
    unsigned Word = MF.getInfo<SquallMachineFunctionInfo>()->getVarArgsWord();
    SDValue Address(
        DAG.getMachineNode(Squall::FIADDR, DL, MVT::i32,
                           DAG.getTargetConstant(Word, DL, MVT::i32),
                           DAG.getTargetConstant(0, DL, MVT::i32)),
        0);
    const Value *List = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
    return DAG.getStore(Op.getOperand(0), DL, Address, Op.getOperand(1),
                        MachinePointerInfo(List));
  }
  default:
    llvm_unreachable("unexpected custom lowering");
  }
}

SDValue SquallTargetLowering::lowerGlobalAddress(SDValue Op,
                                                 SelectionDAG &DAG) const {
  SDLoc DL(Op);
  const auto *N = cast<GlobalAddressSDNode>(Op);
  // A function's address is its entry word: a call tag with frame pointer 0.
  if (const auto *F = dyn_cast<Function>(N->getGlobal());
      F && F->isDeclaration()) {
    unsupported(DAG, DL,
                Twine("'") + F->getName() +
                    "' has no definition in this program");
    return DAG.getConstant(0, DL, MVT::i32);
  }
  SDValue Target =
      DAG.getTargetGlobalAddress(N->getGlobal(), DL, MVT::i32, N->getOffset());
  return DAG.getNode(SquallISD::WRAPPER, DL, MVT::i32, Target);
}

namespace {
/// The cell holding a byte address, and the bit position of that byte in it.
struct CellAddress {
  SDValue Cell, Shift;
};
} // namespace

// Frame objects and globals start on cell boundaries, so the byte position of
// a constant offset from one is known statically.
static CellAddress splitAddress(SelectionDAG &DAG, const SDLoc &DL,
                                SDValue Ptr) {
  SDValue Base = Ptr;
  int64_t Offset = 0;
  if (DAG.isBaseWithConstantOffset(Ptr)) {
    Base = Ptr.getOperand(0);
    Offset = cast<ConstantSDNode>(Ptr.getOperand(1))->getSExtValue();
  }
  bool Aligned = false;
  const GlobalAddressSDNode *G = nullptr;
  if (Base.getOpcode() == SquallISD::WRAPPER)
    G = cast<GlobalAddressSDNode>(Base.getOperand(0));
  else
    G = dyn_cast<GlobalAddressSDNode>(Base);
  if (G && !isa<GlobalVariable>(G->getGlobal()))
    G = nullptr;
  if (G) {
    Offset += G->getOffset();
    Base = DAG.getNode(
        SquallISD::WRAPPER, DL, MVT::i32,
        DAG.getTargetGlobalAddress(G->getGlobal(), DL, MVT::i32, 0));
    Aligned = true;
  } else if (const auto *FI = dyn_cast<FrameIndexSDNode>(Base)) {
    MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
    MFI.setObjectAlignment(
        FI->getIndex(), std::max(MFI.getObjectAlign(FI->getIndex()), Align(4)));
    Aligned = true;
  }
  if (Aligned) {
    SDValue Cell = Base;
    if (int64_t CellOffset = Offset & ~int64_t(3))
      Cell = DAG.getNode(ISD::ADD, DL, MVT::i32, Base,
                         DAG.getConstant(CellOffset, DL, MVT::i32));
    return {Cell, DAG.getConstant((Offset & 3) * 8, DL, MVT::i32)};
  }
  SDValue Cell = DAG.getNode(ISD::AND, DL, MVT::i32, Ptr,
                             DAG.getSignedConstant(-4, DL, MVT::i32));
  SDValue Byte = DAG.getNode(ISD::AND, DL, MVT::i32, Ptr,
                             DAG.getConstant(3, DL, MVT::i32));
  return {Cell, DAG.getNode(ISD::SHL, DL, MVT::i32, Byte,
                            DAG.getConstant(3, DL, MVT::i32))};
}

SDValue SquallTargetLowering::lowerNarrowLoad(SDValue Op,
                                              SelectionDAG &DAG) const {
  auto *LD = cast<LoadSDNode>(Op);
  SDLoc DL(Op);
  EVT MemVT = LD->getMemoryVT();
  unsigned Bits = MemVT.getSizeInBits();
  SDValue Chain = LD->getChain(), Ptr = LD->getBasePtr();
  MachineMemOperand::Flags Flags = LD->getMemOperand()->getFlags();

  // A misaligned i16 may straddle two cells; assemble it from its bytes.
  if (LD->getAlign().value() * 8 < Bits) {
    SDValue Lo = DAG.getExtLoad(ISD::ZEXTLOAD, DL, MVT::i32, Chain, Ptr,
                                LD->getPointerInfo(), MVT::i8, Align(1), Flags);
    SDValue Hi = DAG.getExtLoad(
        ISD::EXTLOAD, DL, MVT::i32, Chain,
        DAG.getObjectPtrOffset(DL, Ptr, TypeSize::getFixed(1)),
        LD->getPointerInfo().getWithOffset(1), MVT::i8, Align(1), Flags);
    SDValue Value = DAG.getNode(ISD::OR, DL, MVT::i32, Lo,
                                DAG.getNode(ISD::SHL, DL, MVT::i32, Hi,
                                            DAG.getConstant(8, DL, MVT::i32)));
    if (LD->getExtensionType() == ISD::SEXTLOAD)
      Value = DAG.getNode(ISD::SIGN_EXTEND_INREG, DL, MVT::i32, Value,
                          DAG.getValueType(MemVT));
    else if (LD->getExtensionType() == ISD::ZEXTLOAD)
      Value = DAG.getZeroExtendInReg(Value, DL, MemVT);
    SDValue Chains = DAG.getNode(ISD::TokenFactor, DL, MVT::Other,
                                 Lo.getValue(1), Hi.getValue(1));
    return DAG.getMergeValues({Value, Chains}, DL);
  }

  CellAddress A = splitAddress(DAG, DL, Ptr);
  SDValue Word = DAG.getLoad(MVT::i32, DL, Chain, A.Cell, MachinePointerInfo(),
                             Align(4), Flags);
  SDValue Value;
  if (LD->getExtensionType() == ISD::SEXTLOAD) {
    // Move the field to the top of the cell, then shift it back down.
    SDValue Up = DAG.getNode(ISD::SUB, DL, MVT::i32,
                             DAG.getConstant(32 - Bits, DL, MVT::i32), A.Shift);
    Value = DAG.getNode(ISD::SRA, DL, MVT::i32,
                        DAG.getNode(ISD::SHL, DL, MVT::i32, Word, Up),
                        DAG.getConstant(32 - Bits, DL, MVT::i32));
  } else {
    Value = DAG.getNode(ISD::SRL, DL, MVT::i32, Word, A.Shift);
    Value = DAG.getZeroExtendInReg(Value, DL, MemVT);
  }
  return DAG.getMergeValues({Value, Word.getValue(1)}, DL);
}

// SquallLowerMemory rewrites partial-cell stores in IR, where their order is
// fixed. This handles only stores that legalization itself creates, which are
// never on independent chains to the same cell.
SDValue SquallTargetLowering::lowerNarrowStore(SDValue Op,
                                               SelectionDAG &DAG) const {
  auto *ST = cast<StoreSDNode>(Op);
  SDLoc DL(Op);
  EVT MemVT = ST->getMemoryVT();
  unsigned Bits = MemVT.getSizeInBits();
  SDValue Chain = ST->getChain(), Ptr = ST->getBasePtr(),
          Value = ST->getValue();
  MachineMemOperand::Flags Flags = ST->getMemOperand()->getFlags();

  if (ST->getAlign().value() * 8 < Bits) {
    SDValue Lo = DAG.getTruncStore(Chain, DL, Value, Ptr, ST->getPointerInfo(),
                                   MVT::i8, Align(1), Flags);
    SDValue High = DAG.getNode(ISD::SRL, DL, MVT::i32, Value,
                               DAG.getConstant(8, DL, MVT::i32));
    // Chained after the low byte: both may update the same cell.
    return DAG.getTruncStore(
        Lo, DL, High, DAG.getObjectPtrOffset(DL, Ptr, TypeSize::getFixed(1)),
        ST->getPointerInfo().getWithOffset(1), MVT::i8, Align(1), Flags);
  }

  CellAddress A = splitAddress(DAG, DL, Ptr);
  // The store is chained after the load, so no other access to the cell can
  // come between them.
  SDValue Word = DAG.getLoad(MVT::i32, DL, Chain, A.Cell, MachinePointerInfo(),
                             Align(4), Flags & ~MachineMemOperand::MOStore);
  SDValue Mask =
      DAG.getConstant(maskTrailingOnes<uint32_t>(Bits), DL, MVT::i32);
  SDValue Hole = DAG.getNOT(
      DL, DAG.getNode(ISD::SHL, DL, MVT::i32, Mask, A.Shift), MVT::i32);
  SDValue Field =
      DAG.getNode(ISD::SHL, DL, MVT::i32,
                  DAG.getNode(ISD::AND, DL, MVT::i32, Value, Mask), A.Shift);
  SDValue Merged =
      DAG.getNode(ISD::OR, DL, MVT::i32,
                  DAG.getNode(ISD::AND, DL, MVT::i32, Word, Hole), Field);
  return DAG.getStore(Word.getValue(1), DL, Merged, A.Cell,
                      MachinePointerInfo(), Align(4), Flags);
}

SDValue SquallTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  if (Ins.size() > Squall::NumArgRegs)
    unsupported(DAG, DL, "Squall functions take at most 16 scalar arguments");
  if (IsVarArg)
    MF.getInfo<SquallMachineFunctionInfo>()->setVarArgsWord(
        Squall::FirstRegWord + Ins.size());
  for (const ISD::InputArg &In : Ins) {
    if (In.VT != MVT::i32) {
      unsupported(DAG, DL, "Squall arguments must be 32-bit scalars");
      InVals.push_back(DAG.getUNDEF(In.VT));
      continue;
    }
    if (InVals.size() >= Squall::NumArgRegs) {
      InVals.push_back(DAG.getUNDEF(In.VT));
      continue;
    }
    Register VReg = MF.addLiveIn(Squall::GPRRegClass.getRegister(InVals.size()),
                                 &Squall::GPRRegClass);
    InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, MVT::i32));
  }
  return Chain;
}

SDValue
SquallTargetLowering::LowerCall(CallLoweringInfo &CLI,
                                SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &DL = CLI.DL;
  CLI.IsTailCall = false;

  // A direct call names a function defined in this module. Any other callee
  // value is a function pointer, the callee's entry word, called indirectly.
  const Function *Callee = nullptr;
  if (auto *G = dyn_cast<GlobalAddressSDNode>(CLI.Callee))
    Callee = dyn_cast<Function>(G->getGlobal());
  std::string Undefined;
  // Calls that code generation introduces (libcalls) name their target; the
  // runtime library linked into the program defines it.
  if (auto *S = dyn_cast<ExternalSymbolSDNode>(CLI.Callee)) {
    const Module *M = DAG.getMachineFunction().getFunction().getParent();
    Callee = M->getFunction(S->getSymbol());
    if (!Callee || Callee->isDeclaration())
      Undefined = S->getSymbol();
  } else if (Callee && Callee->isDeclaration()) {
    Undefined = Callee->getName().str();
  }
  if (!Undefined.empty()) {
    unsupported(DAG, DL,
                Twine("call to '") + Undefined +
                    "' has no definition in this program");
    for (const ISD::InputArg &In : CLI.Ins)
      InVals.push_back(DAG.getUNDEF(In.VT));
    return CLI.Chain;
  }
  if (CLI.Outs.size() > Squall::NumArgRegs || CLI.Ins.size() > 1)
    unsupported(DAG, DL, "unsupported Squall call signature");

  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  SDValue Chain = CLI.Chain;
  SmallVector<SDValue, 16> Args;
  for (unsigned I = 0; I < CLI.OutVals.size() && I < Squall::NumArgRegs; ++I) {
    ISD::ArgFlagsTy Flags = CLI.Outs[I].Flags;
    if (!Flags.isByVal()) {
      Args.push_back(CLI.OutVals[I]);
      continue;
    }
    // The callee owns its copy: a caller frame object per call site, which
    // outlives the call because the caller's frame does. The copy is written
    // a whole cell at a time, so it needs no read-modify-write.
    uint64_t Size = Flags.getByValSize();
    Align SrcAlign = Flags.getNonZeroByValAlign();
    int FI =
        MFI.CreateStackObject(alignTo(Size, 4), std::max(SrcAlign, Align(4)),
                              /*isSpillSlot=*/false);
    SDValue Copy = DAG.getFrameIndex(FI, MVT::i32);
    for (uint64_t Offset = 0; Offset < Size; Offset += 4) {
      SDValue Src = DAG.getObjectPtrOffset(DL, CLI.OutVals[I],
                                           TypeSize::getFixed(Offset));
      SmallVector<SDValue, 4> Loads;
      SDValue Cell;
      if (Offset + 4 <= Size) {
        Cell = DAG.getLoad(MVT::i32, DL, Chain, Src, MachinePointerInfo(),
                           commonAlignment(SrcAlign, Offset));
        Loads.push_back(Cell.getValue(1));
      } else {
        // The final partial cell: no bytes past the source are read.
        Cell = DAG.getConstant(0, DL, MVT::i32);
        for (uint64_t Byte = 0; Offset + Byte < Size; ++Byte) {
          SDValue Value = DAG.getExtLoad(
              ISD::ZEXTLOAD, DL, MVT::i32, Chain,
              DAG.getObjectPtrOffset(DL, Src, TypeSize::getFixed(Byte)),
              MachinePointerInfo(), MVT::i8, Align(1));
          Loads.push_back(Value.getValue(1));
          Cell =
              DAG.getNode(ISD::OR, DL, MVT::i32, Cell,
                          DAG.getNode(ISD::SHL, DL, MVT::i32, Value,
                                      DAG.getConstant(8 * Byte, DL, MVT::i32)));
        }
      }
      Chain = DAG.getStore(
          DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Loads), DL, Cell,
          DAG.getObjectPtrOffset(DL, Copy, TypeSize::getFixed(Offset)),
          MachinePointerInfo::getFixedStack(DAG.getMachineFunction(), FI,
                                            Offset),
          Align(4));
    }
    Args.push_back(Copy);
  }

  SmallVector<SDValue, 18> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee ? DAG.getTargetGlobalAddress(Callee, DL, MVT::i32)
                       : CLI.Callee);
  Ops.append(Args.begin(), Args.end());

  if (CLI.Ins.empty())
    return DAG.getNode(SquallISD::CALL, DL, MVT::Other, Ops);
  SDValue Call = DAG.getNode(SquallISD::CALLR, DL,
                             DAG.getVTList(MVT::i32, MVT::Other), Ops);
  InVals.push_back(Call.getValue(0));
  return Call.getValue(1);
}

bool SquallTargetLowering::CanLowerReturn(
    CallingConv::ID, MachineFunction &, bool,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &,
    const Type *) const {
  return Outs.size() <= 1;
}

SDValue
SquallTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID, bool,
                                  const SmallVectorImpl<ISD::OutputArg> &Outs,
                                  const SmallVectorImpl<SDValue> &OutVals,
                                  const SDLoc &DL, SelectionDAG &DAG) const {
  if (Outs.empty())
    return DAG.getNode(SquallISD::RET_VOID, DL, MVT::Other, Chain);
  return DAG.getNode(SquallISD::RET, DL, MVT::Other, Chain, OutVals[0]);
}
