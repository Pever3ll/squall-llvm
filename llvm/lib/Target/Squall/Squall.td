// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
include "llvm/Target/Target.td"
// Logical token accumulator metadata; no encoded register operand exists.
let Namespace = "Squall" in def ACC : Register<"acc">;
def TokenValue : RegisterClass<"Squall", [i64], 64, (add ACC)>;
include "SquallInstrInfo.td"
defm : RemapAllTargetPseudoPointerOperands<TokenValue>;
def SquallInstrInfo : InstrInfo;
def Squall : Target { let InstructionSet = SquallInstrInfo; }
