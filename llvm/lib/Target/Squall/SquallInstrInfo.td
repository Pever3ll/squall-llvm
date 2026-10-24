// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Squall carries values in tokens and matching slots, with no register file.
// Operand positions are shared by the parser, encoder, printer and IR lowering.
def SQImm : Operand<i64>;
class SQ<dag operands> : Instruction {
  let Namespace = "Squall";
  let OutOperandList = (outs);
  let InOperandList = operands;
  let Size = 8;
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
  let AsmString = "";
}
def ALUI : SQ<(ins SQImm:$alu, SQImm:$dest, SQImm:$port, SQImm:$second, SQImm:$value)>;
def ALUS : SQ<(ins SQImm:$alu, SQImm:$dest, SQImm:$port, SQImm:$second, SQImm:$slot, SQImm:$mode, SQImm:$rule)>;
def SEND : SQ<(ins SQImm:$dest, SQImm:$port, SQImm:$slot, SQImm:$mode, SQImm:$rule)>;
def EXTRACT : SQ<(ins SQImm:$dest, SQImm:$port, SQImm:$target, SQImm:$targetport, SQImm:$slot, SQImm:$mode, SQImm:$rule)>;
def WRITE : SQ<(ins SQImm:$address)>;
