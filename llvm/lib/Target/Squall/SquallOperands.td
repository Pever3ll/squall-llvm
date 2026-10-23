// SquallOperand.td

// --- port: the .l / .r suffix on the primary destination -----------------
def PortAsmOperand : AsmOperandClass {
  let Name = "Port";
  let ParserMethod = "parsePort";
  let RenderMethod = "addImmOperands";
  let DiagnosticType = "InvalidPort";
}

def port : Operand<i32> {
  let ParserMatchClass = PortAsmOperand;
  let PrintMethod      = "printPort";
  let DecoderMethod    = "decodePort";
  let OperandType      = "OPERAND_IMMEDIATE";
}

// --- second_port: optional trailing l / r --------------------------------
// Values: 0 = absent (s=0,p2=0), 2 = 'l' (s=1,p2=0), 3 = 'r' (s=1,p2=1).
def SecondPortAsmOperand : AsmOperandClass {
  let Name = "SecondPort";
  let ParserMethod = "parseSecondPort";
  let RenderMethod = "addImmOperands";
  let IsOptional = 1;
  let DefaultMethod = "defaultSecondPort";
  let DiagnosticType = "InvalidSecondPort";
}

def second_port : Operand<i32> {
  let ParserMatchClass = SecondPortAsmOperand;
  let PrintMethod      = "printSecondPort";
  let OperandType      = "OPERAND_IMMEDIATE";
}

// --- idest: (label) or label; 20-bit signed word offset in d.off ---------
def IDestAsmOperand : AsmOperandClass {
  let Name = "IDest";
  let ParserMethod = "parseIDest";
  let RenderMethod = "addIDestOperands";
  let DiagnosticType = "InvalidIDest";
}

def idest : Operand<OtherVT> {
  let ParserMatchClass = IDestAsmOperand;
  let PrintMethod      = "printIDest";
  let EncoderMethod    = "getIDestOpValue";
  let DecoderMethod    = "decodeIDest";
}

def ipayload : Operand<OtherVT> {
  let ParserMatchClass = IDestAsmOperand;
  let PrintMethod      = "printIDest";
  let EncoderMethod    = "getIDestOpValue";
  let DecoderMethod    = "decodeIDest";
}

// --- fdest: [frame] / (code) / absolute; 11-bit addr + 2-bit at ----------
def FDestAsmOperand : AsmOperandClass {
  let Name = "FDest";
  let ParserMethod = "parseFDest";
  let RenderMethod = "addFDestOperands";
  let DiagnosticType = "InvalidFDest";
}

def fdest : Operand<i32> {
  let MIOperandInfo    = (ops i32imm, i32imm);   // addr, at
  let ParserMatchClass = FDestAsmOperand;
  let PrintMethod      = "printFDest";
  let EncoderMethod    = "getFDestOpValue";
  let DecoderMethod    = "decodeFDest";
}

def PtrAsmOperand : AsmOperandClass {
  let Name = "Ptr";
  let ParserMethod = "parsePtr";
  let RenderMethod = "addImmOperands";
  let DiagnosticType = "InvalidPtr";
  let DiagnosticString = "expected absolute word index (29-bit unsigned)";
}

def ptr : Operand<i64>, ImmLeaf<i64, [{ return isUInt<29>(Imm); }]> {
  let ParserMatchClass = PtrAsmOperand;
  let PrintMethod      = "printPtr";
  let EncoderMethod    = "getPtrOpValue";
  let DecoderMethod    = "decodePtr";
  let OperandType      = "OPERAND_IMMEDIATE";
}

def SImm32AsmOperand : AsmOperandClass {
  let Name = "SImm32";
  let ParserMethod = "parseSImm32";
  let RenderMethod = "addImmOperands";
  let DiagnosticType = "InvalidSImm32";
}

def simm32 : Operand<i64> {
  let ParserMatchClass = SImm32AsmOperand;
  let PrintMethod      = "printSImm32";
  let EncoderMethod    = "getImm32OpValue";
  let OperandType      = "OPERAND_IMMEDIATE";
}
