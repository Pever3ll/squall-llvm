// Squall Instruction Info

include "SquallOperands.td"
include "SquallInstrFormats.td"

// ALU OPS

defm NOP  : ALUOp<"nop", 0>;

defm ADD  : ALUOp<"add", 4>;

defm SUB  : ALUOp<"sub", 5>;

defm MUL  : ALUOp<"mul", 6>;

defm DIV  : ALUOp<"div", 7>;

defm DIVU : ALUOp<"divu", 8>;

defm REM  : ALUOp<"rem", 9>;

defm MOD  : ALUOp<"mod", 10>;

defm AND  : ALUOp<"and", 16>;

defm OR   : ALUOp<"or", 17>;

defm XOR  : ALUOp<"xor", 18>;

defm SHL  : ALUOp<"shl", 19>;

defm SHRL : ALUOp<"shrl", 20>;

defm SHRA : ALUOp<"shra", 21>;

defm NOT  : ALUOp<"not", 24>;

defm REV  : ALUOp<"rev", 25>;

defm DUPL : ALUOp<"dupl", 26>;

defm DUPR : ALUOp<"dupr", 27>;

defm BEQZ : ALUOp<"beqz", 32>;

defm BEQL : ALUOp<"beql", 33>;

defm BEQR : ALUOp<"beqr", 34>;



let mr = 0 in
def EXTR    : ExtractInstr<"extr\t$dst$port $ipayload$portm $slot", []>;

let mr = 2 in
def EXTR_R  : ExtractInstr<"extr.r\t$dst$port $ipayload$portm $slot", []>;

let mr = 1 in
def EXTR_C  : ExtractInstr<"extr.c\t$dst$port $ipayload$portm $slot", []>;



let mr = 0 in
def SEND    : SendInstr<"send\t$dst$port $slot", []>;

let mr = 2 in
def SEND_R  : SendInstr<"send.r\t$dst$port $slot", []>;

let mr = 1 in
def SEND_C  : SendInstr<"send.c\t$dst$port $slot", []>;

//===----------------------------------------------------------------------===//
// WRITE:  write <ptr>
//===----------------------------------------------------------------------===//

def WRITE : SquallInstr<(outs), (ins ptr:$ptr), "write\t$ptr", []> {
    bits<29> ptr;

    let type = 0b10;
    let Inst{63-31} = 0;
    let Inst{30-2} = ptr;
}


