add_llvm_component_group(Squall)

set(LLVM_TARGET_DEFINITIONS Squall.td)

tablegen(LLVM SquallGenAsmMatcher.inc   -gen-asm-matcher)
tablegen(LLVM SquallGenAsmWriter.inc    -gen-asm-writer)
tablegen(LLVM SquallGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM SquallGenInstrInfo.inc    -gen-instr-info)
tablegen(LLVM SquallGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM SquallGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM SquallGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(SquallCommonTableGen)

add_subdirectory(AsmParser)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)


add_llvm_target(SquallCodeGen
  SquallTargetMachine.cpp

  LINK_COMPONENTS
  CodeGen Core MC SquallDesc SquallInfo Support Target

  ADD_TO_COMPONENT
  Squall
)
