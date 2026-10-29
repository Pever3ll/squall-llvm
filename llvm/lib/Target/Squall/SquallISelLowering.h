//===-- SquallISelLowering.h - Squall DAG lowering --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALLISELLOWERING_H
#define LLVM_LIB_TARGET_SQUALL_SQUALLISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class SquallSubtarget;

class SquallTargetLowering : public TargetLowering {
public:
  SquallTargetLowering(const TargetMachine &TM, const SquallSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;
  SDValue LowerCall(CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;
  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;
  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  // Memory is written a whole cell at a time. SquallLowerMemory has already
  // turned partial-cell stores into ordered cell updates; combines must not
  // narrow those back into stores that would each need a read-modify-write.
  bool allowsMemoryAccess(LLVMContext &Context, const DataLayout &DL, EVT VT,
                          unsigned AddrSpace, Align Alignment,
                          MachineMemOperand::Flags Flags,
                          unsigned *Fast) const override {
    if ((Flags & MachineMemOperand::MOStore) && VT.getStoreSize() < 4)
      return false;
    return TargetLowering::allowsMemoryAccess(Context, DL, VT, AddrSpace,
                                              Alignment, Flags, Fast);
  }

  // A narrow load costs a cell load plus extraction; keep the full cell.
  bool shouldReduceLoadWidth(SDNode *, ISD::LoadExtType, EVT,
                             std::optional<unsigned>) const override {
    return false;
  }

  // Calls are selected directly; no call frame pseudos are needed because a
  // callee's frame is addressed relative to the caller's final frame size.
  bool mayBeEmittedAsTailCall(const CallInst *) const override { return false; }

private:
  SDValue lowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerNarrowLoad(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerNarrowStore(SDValue Op, SelectionDAG &DAG) const;
};

} // namespace llvm

#endif
