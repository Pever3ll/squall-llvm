add_llvm_component_library(LLVMSquallDesc
  SquallMCTargetDesc.cpp
  SquallAsmBackend.cpp
  LINK_COMPONENTS
  MC
  SquallInfo
  Support
  ADD_TO_COMPONENT Squall
)
