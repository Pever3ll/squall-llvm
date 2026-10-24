//===-- SquallRegisterInfo.td - Squall frame-slot registers -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall has no register file. Every activation owns a window of matching-slot
// words addressed relative to the token's frame pointer, and an instruction can
// read any of them as its second operand. The backend therefore models frame
// words as registers: S<n> is frame word <n> + 3. Words 0-2 hold the return
// tag and two expansion scratch slots and are never allocated.
//
// Frames are private to an activation, so a call cannot clobber any register.
//
//===----------------------------------------------------------------------===//

class SquallReg<bits<16> Word, string Name> : Register<Name> {
  let HWEncoding = Word;
  let Namespace = "Squall";
}

foreach I = 0-127 in
  def S#I : SquallReg<!add(I, 3), "s"#I>;

// Debug-information frame register; never allocated or referenced by code.
def FP : SquallReg<0, "fp">;

def GPR : RegisterClass<"Squall", [i32], 32, (sequence "S%u", 0, 127)>;
def FPR : RegisterClass<"Squall", [i32], 32, (add FP)> {
  let isAllocatable = 0;
}
