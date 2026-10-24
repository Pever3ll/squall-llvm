add_llvm_component_library(LLVMSquallDisassembler
  SquallDisassembler.cpp
  LINK_COMPONENTS MC MCDisassembler SquallDesc SquallInfo Support
  ADD_TO_COMPONENT Squall
)
