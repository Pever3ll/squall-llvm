add_llvm_component_library(LLVMSquallInfo
  SquallTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Squall
)
