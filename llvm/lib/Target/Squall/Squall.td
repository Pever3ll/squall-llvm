//===----------------------------------------------------------------------===//
// Squall target definition
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Registers - Squall is a dataflow machine with no architectural registers,
// but MCRegisterInfo must exist, so declare an empty class.
//===----------------------------------------------------------------------===//
def DUMMY : Register<"dummy">;

def SquallRegClass : RegisterClass<"Squall", [i64], 64, (add DUMMY)>;

defm : RemapAllTargetPseudoPointerOperands<SquallRegClass>;

//===----------------------------------------------------------------------===//
// Instruction info
//===----------------------------------------------------------------------===//
include "SquallInstrInfo.td"

def SquallInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Subtarget
//===----------------------------------------------------------------------===//
def : ProcessorModel<"generic", NoSchedModel, []>;

//===----------------------------------------------------------------------===//
// Assembly parser / printer
//===----------------------------------------------------------------------===//
def SquallAsmParser : AsmParser {
  // No architectural registers, so suppress MatchRegisterName generation.
  let ShouldEmitMatchRegisterName = 0;
}

def SquallAsmParserVariant : AsmParserVariant {
  let RegisterPrefix = "";
}

def SquallAsmWriter : AsmWriter {
  let AsmWriterClassName  = "InstPrinter";
  let PassSubtarget       = 1;
  let Variant             = 0;
}

//===----------------------------------------------------------------------===//
// Target
//===----------------------------------------------------------------------===//
def Squall : Target {
  let InstructionSet      = SquallInstrInfo;
  let AssemblyParsers     = [SquallAsmParser];
  let AssemblyParserVariants = [SquallAsmParserVariant];
  let AssemblyWriters     = [SquallAsmWriter];
}
