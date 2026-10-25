add_llvm_component_group(Squall)

set(LLVM_TARGET_DEFINITIONS Squall.td)

tablegen(LLVM SquallGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM SquallGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM SquallGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM SquallGenSDNodeInfo.inc -gen-sd-node-info)
tablegen(LLVM SquallGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(SquallCommonTableGen)

add_llvm_target(SquallCodeGen
  SquallAsmPrinter.cpp
  SquallCheckIR.cpp
  SquallForkJoin.cpp
  SquallFrameLowering.cpp
  SquallISelDAGToDAG.cpp
  SquallISelLowering.cpp
  SquallLowerMemory.cpp
  SquallInstrInfo.cpp
  SquallRegisterInfo.cpp
  SquallSelectionDAGInfo.cpp
  SquallSubtarget.cpp
  SquallTargetMachine.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  SelectionDAG
  SquallDesc
  SquallInfo
  Support
  Target
  TargetParser
  TransformUtils

  ADD_TO_COMPONENT
  Squall
)

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
