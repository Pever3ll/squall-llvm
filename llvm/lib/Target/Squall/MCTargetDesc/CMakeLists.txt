add_llvm_component_library(LLVMSquallDesc
  SquallMCAsmInfo.cpp
  SquallInstPrinter.cpp
  SquallMCCodeEmitter.cpp
  SquallAsmBackend.cpp
  SquallELFObjectWriter.cpp
  SquallMCTargetDesc.cpp

  DEPENDS
  intrinsics_gen
  SquallCommonTableGen

  ADD_TO_COMPONENT
  Squall
  )
