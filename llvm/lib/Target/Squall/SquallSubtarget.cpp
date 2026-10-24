//===-- SquallSubtarget.cpp - Squall subtarget --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SquallSubtarget.h"
#include "SquallSelectionDAGInfo.h"

using namespace llvm;

#define DEBUG_TYPE "squall-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "SquallGenSubtargetInfo.inc"

SquallSubtarget::SquallSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                                 const TargetMachine &TM)
    : SquallGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS), InstrInfo(*this),
      TLInfo(TM, *this), TSInfo(std::make_unique<SquallSelectionDAGInfo>()) {}

SquallSubtarget::~SquallSubtarget() = default;
