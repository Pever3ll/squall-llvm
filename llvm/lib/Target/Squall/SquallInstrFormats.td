// Squall Instruction Formats


class SquallInstr<dag outs, dag ins, string asmstr, list<dag> pattern, 
            InstrItinClass itin = NoItinerary>
        : Instruction {
    field bits<64> Inst;

    let Namespace = "Squall";
    let Size = 8;

    bits<2> type;

    let Inst{1-0} = type;           // Monadic, Dyadic, or Write

    let OutOperandList = outs;
    let InOperandList = ins;
    let AsmString = asmstr;
    let Pattern = pattern;
    let Itinerary = itin;
}

class GenInstr<dag outs, dag ins, string asmstr, list<dag> pattern>
        : SquallInstr<outs, ins, asmstr, pattern> {
    bit port;
    bits<20> dst;
    let Inst{63} = port;
    let Inst{62-43} = dst;
}

class MonadicInstr<dag outs, dag ins, string asmstr, list<dag> pattern>
        : GenInstr<outs, ins, asmstr, pattern> {
    bits<32> imm;
    let Inst{41-10} = imm;
    let type = 0b00;
}

class DyadicInstr<dag outs, dag ins, string asmstr, list<dag> pattern>
        : GenInstr<outs, ins, asmstr, pattern> {
    bits<2> mr;
    bits<13> slot;
    bits<2> dyad_type;
    let type = 0b01;
    let Inst{42-41} = mr;
    let Inst{40-39} = slot{12-11};
    let Inst{38-28} = slot{10-0};
    let Inst{27-26} = dyad_type;
}

class ALUType<bits<6> opcode> {
    bits<2> p2s;
    bits<6> opc;
    let opc = opcode;
}


//===----------------------------------------------------------------------===//
// Dyadic ALU:  <instr>(.r|.c)? (idest).(l|r) (l|r)? [fdest]
//===----------------------------------------------------------------------===//
class DyadicALU<dag outs, dag ins, string asmstr, list<dag> pattern,
                bits<6> opcode>
        : DyadicInstr<outs, ins, asmstr, pattern>, ALUType<opcode> {
    let Inst{25-10} = 0;            // reserved
    let Inst{9}     = p2s{1};
    let Inst{8}     = p2s{0};
    let Inst{7-2}   = opc;
    let dyad_type = 0b00;
}

//===----------------------------------------------------------------------===//
// Monadic ALU:  <instr> (idest).(l|r) (l|r)? #imm
//===----------------------------------------------------------------------===//
class MonadicALU<dag outs, dag ins, string asmstr, list<dag> pattern,
                 bits<6> opcode>
        : GenInstr<outs, ins, asmstr, pattern>, ALUType<opcode> {
    bits<32> imm;
    let type = 0b00;

    let Inst{42}    = 0;
    let Inst{41-10} = imm;
    let Inst{9}     = p2s{1};
    let Inst{8}     = p2s{0};
    let Inst{7-2}   = opc;
}

//===----------------------------------------------------------------------===//
// EXTR:  extr(\.(r|c))? (\(<idest>\)|<label>)\.(l|r) (\(<ipayload>\)|<label>)\.(l|r) (\[<fdest>\]|\(<fdest>\)|<fdest>)
//===----------------------------------------------------------------------===//

class ExtractInstr<string asmstr, list<dag> pattern>
        : DyadicInstr<(outs), (ins idest:$dst, port:$port,
                                ipayload:$ipayload, port:$portm,
                                fdest:$slot), asmstr, pattern> {
    bit portm;
    bits<20> ipayload;
    let dyad_type   = 0b10;
    let Inst{25-23} = 0;
    let Inst{22}    = portm;
    let Inst{21-2}  = ipayload;
}

//===----------------------------------------------------------------------===//
// SEND:  send(\.(r|c))? (\(<idest>\)|<label>)\.(l|r) (.l|.r)? (\[<fdest>\]|\(<fdest>\)|<fdest>)
//===----------------------------------------------------------------------===//

class SendInstr<string asmstr, list<dag> pattern>
        : DyadicInstr<(outs), (ins idest:$dst, port:$port,
                                fdest:$slot), asmstr, pattern> {
    let dyad_type  = 0b11;
    let Inst{25-2} = 0;
}

multiclass ALUOp<string mn, bits<6> opc> {
    let mr = 0 in
    def _D  : DyadicALU<(outs),
                      (ins idest:$dst, port:$port, second_port:$p2s,
                           fdest:$slot),
                      mn # "\t$dst$port$p2s $slot", [], opc>;

    let mr = 2 in
    def _DR  : DyadicALU<(outs),
                      (ins idest:$dst, port:$port, second_port:$p2s,
                           fdest:$slot),
                      mn # ".r\t$dst$port$p2s $slot", [], opc>;

    let mr = 1 in
    def _DC  : DyadicALU<(outs),
                      (ins idest:$dst, port:$port, second_port:$p2s,
                           fdest:$slot),
                      mn # ".c\t$dst$port$p2s $slot", [], opc>;
    def _M   : MonadicALU<(outs),
                      (ins idest:$dst, port:$port, second_port:$p2s,
                           simm32:$imm),
                      mn # "\t$dst$port$p2s #$imm", [], opc>;
}


