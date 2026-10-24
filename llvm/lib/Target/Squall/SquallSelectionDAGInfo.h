//===-- SquallSelectionDAGInfo.h - Squall target DAG nodes ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SQUALL_SQUALLSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_SQUALL_SQUALLSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "SquallGenSDNodeInfo.inc"

namespace llvm {

class SquallSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  SquallSelectionDAGInfo();
  ~SquallSelectionDAGInfo() override;
};

} // namespace llvm

#endif
