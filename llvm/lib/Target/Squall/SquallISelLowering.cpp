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
// Calling convention: argument I is written to the callee's S<I>; the return
// value is the token value delivered to the caller's continuation. Calls
// preserve every register because each activation has a private frame. A
// byval argument is copied into a caller frame object, whose address is passed.
//
//===----------------------------------------------------------------------===//

#include "SquallISelLowering.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
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

  // Memory intrinsics are expanded inline into four-byte cell accesses; there
  // is no runtime library to call.
  MaxStoresPerMemcpy = MaxStoresPerMemcpyOptSize = 256;
  MaxStoresPerMemmove = MaxStoresPerMemmoveOptSize = 256;
  MaxStoresPerMemset = MaxStoresPerMemsetOptSize = 256;

  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
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
  for (MVT VT : {MVT::i1, MVT::i8, MVT::i16}) {
    setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, VT, Expand);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Expand);
    setTruncStoreAction(MVT::i32, VT, Expand);
  }
}

static void unsupported(SelectionDAG &DAG, const SDLoc &DL, const Twine &What) {
  DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
      DAG.getMachineFunction().getFunction(), What, DL.getDebugLoc()));
}

SDValue SquallTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  assert(Op.getOpcode() == ISD::GlobalAddress && "unexpected custom lowering");
  SDLoc DL(Op);
  const auto *N = cast<GlobalAddressSDNode>(Op);
  if (!isa<GlobalVariable>(N->getGlobal())) {
    unsupported(DAG, DL, "function pointers are not supported on Squall");
    return DAG.getConstant(0, DL, MVT::i32);
  }
  SDValue Target =
      DAG.getTargetGlobalAddress(N->getGlobal(), DL, MVT::i32, N->getOffset());
  return DAG.getNode(SquallISD::WRAPPER, DL, MVT::i32, Target);
}

SDValue SquallTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  if (IsVarArg || Ins.size() > Squall::NumArgRegs)
    unsupported(DAG, DL,
                "Squall functions take at most 16 fixed scalar arguments");
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

  const Function *Callee = nullptr;
  if (auto *G = dyn_cast<GlobalAddressSDNode>(CLI.Callee))
    Callee = dyn_cast<Function>(G->getGlobal());
  if (!Callee || Callee->isDeclaration()) {
    std::string Name;
    if (auto *S = dyn_cast<ExternalSymbolSDNode>(CLI.Callee))
      Name = S->getSymbol();
    else if (Callee)
      Name = Callee->getName().str();
    unsupported(DAG, DL,
                Name.empty()
                    ? Twine("indirect calls are not supported on Squall")
                    : Twine("call to '") + Name +
                          "' has no definition in this module; Squall has no "
                          "linker or runtime library");
    for (const ISD::InputArg &In : CLI.Ins)
      InVals.push_back(DAG.getUNDEF(In.VT));
    return CLI.Chain;
  }
  if (CLI.IsVarArg || CLI.Outs.size() > Squall::NumArgRegs ||
      CLI.Ins.size() > 1)
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
    // outlives the call because the caller's frame does.
    Align A = std::max(Flags.getNonZeroByValAlign(), Align(4));
    int FI = MFI.CreateStackObject(alignTo(Flags.getByValSize(), 4), A,
                                   /*isSpillSlot=*/false);
    SDValue Copy = DAG.getFrameIndex(FI, MVT::i32);
    Chain = DAG.getMemcpy(
        Chain, DL, Copy, CLI.OutVals[I],
        DAG.getConstant(Flags.getByValSize(), DL, MVT::i32), A, A,
        /*isVol=*/false, /*AlwaysInline=*/true, /*CI=*/nullptr, std::nullopt,
        MachinePointerInfo::getFixedStack(DAG.getMachineFunction(), FI),
        MachinePointerInfo());
    Args.push_back(Copy);
  }

  SmallVector<SDValue, 18> Ops;
  Ops.push_back(Chain);
  Ops.push_back(DAG.getTargetGlobalAddress(Callee, DL, MVT::i32));
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
