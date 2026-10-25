//===-- SquallISelDAGToDAG.cpp - Squall instruction selection -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallSelectionDAGInfo.h"
#include "SquallTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"

using namespace llvm;

#define DEBUG_TYPE "squall-isel"
#define PASS_NAME "Squall DAG->DAG Pattern Instruction Selection"

namespace {

class SquallDAGToDAGISel : public SelectionDAGISel {
public:
  SquallDAGToDAGISel(SquallTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISel(TM, OptLevel) {}

private:
#include "SquallGenDAGISel.inc"

  void Select(SDNode *N) override;
  void selectCall(SDNode *N);

  bool selectAddrFI(SDValue Addr, SDValue &Base, SDValue &Offset);
  bool selectAddrG(SDValue Addr, SDValue &Global);
};

class SquallDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  SquallDAGToDAGISelLegacy(SquallTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISelLegacy(
            ID, std::make_unique<SquallDAGToDAGISel>(TM, OptLevel)) {}
};

} // namespace

char SquallDAGToDAGISelLegacy::ID;

INITIALIZE_PASS(SquallDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createSquallISelDag(SquallTargetMachine &TM,
                                        CodeGenOptLevel OptLevel) {
  return new SquallDAGToDAGISelLegacy(TM, OptLevel);
}

bool SquallDAGToDAGISel::selectAddrFI(SDValue Addr, SDValue &Base,
                                      SDValue &Offset) {
  SDLoc DL(Addr);
  int64_t Bytes = 0;
  if (CurDAG->isBaseWithConstantOffset(Addr)) {
    Bytes = cast<ConstantSDNode>(Addr.getOperand(1))->getSExtValue();
    Addr = Addr.getOperand(0);
  }
  auto *FI = dyn_cast<FrameIndexSDNode>(Addr);
  if (!FI || Bytes < 0 || Bytes % 4)
    return false;
  Base = CurDAG->getTargetFrameIndex(FI->getIndex(), MVT::i32);
  Offset = CurDAG->getTargetConstant(Bytes, DL, MVT::i32);
  return true;
}

bool SquallDAGToDAGISel::selectAddrG(SDValue Addr, SDValue &Global) {
  int64_t Bytes = 0;
  if (CurDAG->isBaseWithConstantOffset(Addr)) {
    Bytes = cast<ConstantSDNode>(Addr.getOperand(1))->getSExtValue();
    Addr = Addr.getOperand(0);
  }
  if (Addr.getOpcode() != SquallISD::WRAPPER)
    return false;
  auto *G = cast<GlobalAddressSDNode>(Addr.getOperand(0));
  Bytes += G->getOffset();
  if (Bytes < 0 || Bytes % 4)
    return false;
  Global = CurDAG->getTargetGlobalAddress(G->getGlobal(), SDLoc(Addr), MVT::i32,
                                          Bytes);
  return true;
}

void SquallDAGToDAGISel::selectCall(SDNode *N) {
  SmallVector<SDValue, 18> Ops;
  for (unsigned I = 1, E = N->getNumOperands(); I != E; ++I)
    Ops.push_back(N->getOperand(I));
  Ops.push_back(N->getOperand(0)); // Chain.
  bool HasResult = N->getOpcode() == SquallISD::CALLR;
  SDVTList VTs = HasResult ? CurDAG->getVTList(MVT::i32, MVT::Other)
                           : CurDAG->getVTList(MVT::Other);
  MachineSDNode *Call = CurDAG->getMachineNode(
      HasResult ? Squall::CALLR : Squall::CALL, SDLoc(N), VTs, Ops);
  ReplaceNode(N, Call);
}

void SquallDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return;
  }
  switch (N->getOpcode()) {
  case ISD::FrameIndex: {
    SDLoc DL(N);
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    CurDAG->SelectNodeTo(N, Squall::FIADDR, MVT::i32,
                         CurDAG->getTargetFrameIndex(FI, MVT::i32),
                         CurDAG->getTargetConstant(0, DL, MVT::i32));
    return;
  }
  case SquallISD::CALL:
  case SquallISD::CALLR:
    selectCall(N);
    return;
  default:
    break;
  }
  SelectCode(N);
}
