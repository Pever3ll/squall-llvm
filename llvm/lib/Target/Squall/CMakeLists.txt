add_llvm_component_group(Squall)

set(LLVM_TARGET_DEFINITIONS Squall.td)

tablegen(LLVM SquallGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM SquallGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM SquallGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM SquallGenAsmMatcher.inc -gen-asm-matcher)

add_public_tablegen_target(SquallCommonTableGen)

add_subdirectory(AsmParser)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
