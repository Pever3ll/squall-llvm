add_llvm_library(LLVMSQUALLInfo
  SquallTargetInfo.cpp

  LINK_COMPONENTS
  Support

  ADD_TO_COMPONENT
  Squall
)
