add_llvm_component_library(LLVMSquallDesc
  SquallMCAsmInfo.cpp
  SquallMCExpr.cpp
  SquallMCTargetDesc.cpp
  SquallInstPrinter.cpp
  SquallMCCodeEmitter.cpp
  SquallAsmBackend.cpp
  SquallELFObjectWriter.cpp

  DEPENDS
  intrinsics_gen
  SquallCommonTableGen

  LINK_COMPONENTS
  MC
  MCParser
  Support

  ADD_TO_COMPONENT
  Squall
)
