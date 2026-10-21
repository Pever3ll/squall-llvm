add_llvm_component_library(LLVMSquallDesc
  SquallMCTargetDesc.cpp
  LINK_COMPONENTS
  MC
  SquallInfo
  Support

  ADD_TO_COMPONENT
  Squall
  )
