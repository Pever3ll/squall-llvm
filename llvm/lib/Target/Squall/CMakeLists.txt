add_llvm_component_group(Squall)

set(LLVM_TARGET_DEFINITIONS Squall.td)
tablegen(LLVM SquallGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM SquallGenRegisterInfo.inc -gen-register-info)
add_public_tablegen_target(SquallCommonTableGen)
add_llvm_target(SquallCodeGen
  SquallTargetMachine.cpp
  SquallLowering.cpp
  LINK_COMPONENTS
  Core
  CodeGen
  MC
  MCParser
  SquallDesc
  SquallInfo
  Support
  Target
  TargetParser
  ADD_TO_COMPONENT
  Squall
)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
