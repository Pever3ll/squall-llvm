add_llvm_component_library(LLVMSquallAsmParser
  SquallAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  SquallDesc
  SquallInfo
  Support

  DEPENDS
  intrinsics_gen
  SquallCommonTableGen

  ADD_TO_COMPONENT
  Squall
)
