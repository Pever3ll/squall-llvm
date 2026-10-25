//===-- Squall.td - Describe the Squall target -------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

include "SquallRegisterInfo.td"
include "SquallInstrInfo.td"

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def : ProcessorModel<"generic", NoSchedModel, []>;

def SquallInstrInfo : InstrInfo;

def Squall : Target {
  let InstructionSet = SquallInstrInfo;
}
