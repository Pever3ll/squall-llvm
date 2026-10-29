//===-- SquallInstrInfo.td - Squall instructions -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two layers of instructions are defined here:
//
//  * Encoded instructions (ALUI, ALUS, SEND, EXTRACT, WRITE). These are the
//    five 64-bit Squall v2 word formats used by the MC layer: the assembler,
//    printer, encoder and disassembler. Every field, including the successor
//    destination, is an explicit operand.
//
//  * Code generation pseudos. Instruction selection and register allocation
//    work on these frame-slot "register" operations. The AsmPrinter expands
//    each one into a chain of encoded instructions whose final token continues
//    at the next word, so expanded code executes in program order with a
//    single control token.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Encoded instruction formats
//===----------------------------------------------------------------------===//

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

def ALUI : SQ<(ins SQImm:$alu, SQImm:$dest, SQImm:$port, SQImm:$second,
                   SQImm:$value)>;
def ALUS : SQ<(ins SQImm:$alu, SQImm:$dest, SQImm:$port, SQImm:$second,
                   SQImm:$slot, SQImm:$mode, SQImm:$rule)>;
def SEND : SQ<(ins SQImm:$dest, SQImm:$port, SQImm:$slot, SQImm:$mode,
                   SQImm:$rule)>;
def EXTRACT : SQ<(ins SQImm:$dest, SQImm:$port, SQImm:$target,
                      SQImm:$targetport, SQImm:$slot, SQImm:$mode,
                      SQImm:$rule)>;
def WRITE : SQ<(ins SQImm:$address)>;

//===----------------------------------------------------------------------===//
// Target DAG nodes
//===----------------------------------------------------------------------===//

def SDT_SquallCall : SDTypeProfile<0, -1, [SDTCisVT<0, i32>]>;
def SDT_SquallCallR : SDTypeProfile<1, -1, [SDTCisVT<0, i32>,
                                            SDTCisVT<1, i32>]>;
def SDT_SquallRet : SDTypeProfile<0, 1, [SDTCisVT<0, i32>]>;
def SDT_SquallWrapper : SDTypeProfile<1, 1, [SDTCisSameAs<0, 1>,
                                             SDTCisPtrTy<0>]>;

// Calls without and with a returned value. The operands are the callee, a
// function or a function pointer, followed by the argument values.
def SquallCall : SDNode<"SquallISD::CALL", SDT_SquallCall,
                        [SDNPHasChain, SDNPVariadic, SDNPSideEffect,
                         SDNPMayLoad, SDNPMayStore]>;
def SquallCallR : SDNode<"SquallISD::CALLR", SDT_SquallCallR,
                         [SDNPHasChain, SDNPVariadic, SDNPSideEffect,
                          SDNPMayLoad, SDNPMayStore]>;
def SquallRetVoid : SDNode<"SquallISD::RET_VOID", SDTNone, [SDNPHasChain]>;
def SquallRet : SDNode<"SquallISD::RET", SDT_SquallRet, [SDNPHasChain]>;
// A global's address; its word is fixed by the image layout.
def SquallWrapper : SDNode<"SquallISD::WRAPPER", SDT_SquallWrapper>;

//===----------------------------------------------------------------------===//
// Operands and addressing modes
//===----------------------------------------------------------------------===//

def brtarget : Operand<OtherVT>;
def calltarget : Operand<i32>;
def globalsym : Operand<i32>;

// A frame object: frame index, then a byte offset into it. Frame index
// elimination rewrites the pair to a frame word and a zero offset.
def memfi : Operand<i32> {
  let MIOperandInfo = (ops i32imm, i32imm);
}

def AddrFI : ComplexPattern<i32, 2, "selectAddrFI", [frameindex, add, or]>;
def AddrG : ComplexPattern<i32, 1, "selectAddrG", [SquallWrapper, add, or]>;

def immNonNeg : ImmLeaf<i32, [{ return Imm >= 0; }]>;

//===----------------------------------------------------------------------===//
// Code generation pseudos
//===----------------------------------------------------------------------===//

class SquallPseudo<dag outs, dag ins, list<dag> pattern = []> : Instruction {
  let Namespace = "Squall";
  let OutOperandList = outs;
  let InOperandList = ins;
  let Pattern = pattern;
  let isPseudo = 1;
  let isCodeGenOnly = 1;
  let AsmString = "";
  let Size = 0;
  let hasSideEffects = 0;
  let mayLoad = 0;
  let mayStore = 0;
}

multiclass BinOp<SDPatternOperator node, SDPatternOperator immediate = imm> {
  def rr : SquallPseudo<(outs GPR:$d), (ins GPR:$a, GPR:$b),
                        [(set GPR:$d, (node GPR:$a, GPR:$b))]>;
  def ri : SquallPseudo<(outs GPR:$d), (ins GPR:$a, i32imm:$i),
                        [(set GPR:$d, (node GPR:$a, immediate:$i))]>;
}

defm ADD : BinOp<add>;
defm SUB : BinOp<sub>;
defm MUL : BinOp<mul>;
defm SDIV : BinOp<sdiv>;
defm SREM : BinOp<srem>;
defm AND : BinOp<and>;
defm OR : BinOp<or>;
defm XOR : BinOp<xor>;
defm SHL : BinOp<shl>;
// High halves of the 64-bit product; used to legalize 64-bit multiplication.
def MULHS : SquallPseudo<(outs GPR:$d), (ins GPR:$a, GPR:$b),
                         [(set GPR:$d, (mulhs GPR:$a, GPR:$b))]>;
def MULHU : SquallPseudo<(outs GPR:$d), (ins GPR:$a, GPR:$b),
                         [(set GPR:$d, (mulhu GPR:$a, GPR:$b))]>;
defm SRA : BinOp<sra>;
// Unsigned operations zero-extend their operands, so an immediate must also
// be nonnegative when read as the signed 32-bit instruction field.
defm UDIV : BinOp<udiv, immNonNeg>;
defm UREM : BinOp<urem, immNonNeg>;
defm SRL : BinOp<srl, immNonNeg>;

// Integer comparison producing 0 or 1. The immediate is a Squall::CondCode.
def SETCC : SquallPseudo<(outs GPR:$d), (ins GPR:$a, GPR:$b, i32imm:$cc)>;
class CmpPat<CondCode CC, int Code>
    : Pat<(setcc GPR:$a, GPR:$b, CC), (SETCC GPR:$a, GPR:$b, (i32 Code))>;
def : CmpPat<SETEQ, 0>;
def : CmpPat<SETNE, 1>;
def : CmpPat<SETLT, 2>;
def : CmpPat<SETLE, 3>;
def : CmpPat<SETGT, 4>;
def : CmpPat<SETGE, 5>;
def : CmpPat<SETULT, 6>;
def : CmpPat<SETULE, 7>;
def : CmpPat<SETUGT, 8>;
def : CmpPat<SETUGE, 9>;

def SELECT : SquallPseudo<(outs GPR:$d), (ins GPR:$c, GPR:$t, GPR:$f),
                          [(set GPR:$d, (select GPR:$c, GPR:$t, GPR:$f))]>;

let isReMaterializable = 1, isAsCheapAsAMove = 1, isMoveImm = 1 in
def MOVi : SquallPseudo<(outs GPR:$d), (ins i32imm:$v),
                        [(set GPR:$d, imm:$v)]>;
let isReMaterializable = 1, isAsCheapAsAMove = 1 in
def MOVG : SquallPseudo<(outs GPR:$d), (ins globalsym:$g),
                        [(set GPR:$d, (SquallWrapper tglobaladdr:$g))]>;
let isMoveReg = 1 in
def MOVrr : SquallPseudo<(outs GPR:$d), (ins GPR:$s)>;
// Address of a frame object; selected from ISD::FrameIndex.
let isReMaterializable = 1 in
def FIADDR : SquallPseudo<(outs GPR:$d), (ins memfi:$a)>;

let mayLoad = 1 in {
  def LDF : SquallPseudo<(outs GPR:$d), (ins memfi:$a),
                         [(set GPR:$d, (load AddrFI:$a))]>;
  def LDG : SquallPseudo<(outs GPR:$d), (ins globalsym:$g),
                         [(set GPR:$d, (load AddrG:$g))]>;
  def LDP : SquallPseudo<(outs GPR:$d), (ins GPR:$p),
                         [(set GPR:$d, (load GPR:$p))]>;
}
let mayStore = 1 in {
  def STF : SquallPseudo<(outs), (ins GPR:$v, memfi:$a),
                         [(store GPR:$v, AddrFI:$a)]>;
  def STG : SquallPseudo<(outs), (ins GPR:$v, globalsym:$g),
                         [(store GPR:$v, AddrG:$g)]>;
  def STP : SquallPseudo<(outs), (ins GPR:$v, GPR:$p),
                         [(store GPR:$v, GPR:$p)]>;
}

let isBranch = 1, isTerminator = 1 in {
  let isBarrier = 1 in
  def BR : SquallPseudo<(outs), (ins brtarget:$t), [(br bb:$t)]>;
  // Branch when the condition is nonzero / zero.
  def BRNZ : SquallPseudo<(outs), (ins GPR:$c, brtarget:$t),
                          [(brcond GPR:$c, bb:$t)]>;
  def BRZ : SquallPseudo<(outs), (ins GPR:$c, brtarget:$t)>;
}

// Direct calls name the callee; indirect calls read a function pointer.
let isCall = 1, hasSideEffects = 1, mayLoad = 1, mayStore = 1 in {
  def CALL : SquallPseudo<(outs), (ins calltarget:$f, variable_ops)>;
  def CALLR : SquallPseudo<(outs GPR:$d), (ins calltarget:$f, variable_ops)>;
  def CALLI : SquallPseudo<(outs), (ins GPR:$f, variable_ops)>;
  def CALLIR : SquallPseudo<(outs GPR:$d), (ins GPR:$f, variable_ops)>;
}

// Halts the program with the trap status (__builtin_trap, abort, assert).
let hasSideEffects = 1 in
def TRAP : SquallPseudo<(outs), (ins), [(trap)]>;

let isReturn = 1, isTerminator = 1, isBarrier = 1 in {
  def RET : SquallPseudo<(outs), (ins), [(SquallRetVoid)]>;
  def RETV : SquallPseudo<(outs), (ins GPR:$v), [(SquallRet GPR:$v)]>;
}

// Parallel lanes (SquallForkJoin): FORK id; lane A; LANE id; lane B;
// JOIN id, slot. The unique id keeps branch folding from merging parts of
// different regions; slot is 0 for the shared join word, or 1 + the index of
// a reserved call join word.
let hasSideEffects = 1, isNotDuplicable = 1 in {
  def FORK : SquallPseudo<(outs), (ins i32imm:$id)>;
  def LANE : SquallPseudo<(outs), (ins i32imm:$id)>;
  def JOIN : SquallPseudo<(outs), (ins i32imm:$id, i32imm:$slot)>;
}
