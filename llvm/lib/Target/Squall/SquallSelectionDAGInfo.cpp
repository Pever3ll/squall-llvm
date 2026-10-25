//===-- SquallSelectionDAGInfo.cpp - Squall target DAG nodes ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallSelectionDAGInfo.h"

#define GET_SDNODE_DESC
#include "SquallGenSDNodeInfo.inc"

using namespace llvm;

SquallSelectionDAGInfo::SquallSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(SquallGenSDNodeInfo) {}

SquallSelectionDAGInfo::~SquallSelectionDAGInfo() = default;
