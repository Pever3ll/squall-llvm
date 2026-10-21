add_llvm_component_group(Squall)

# Native MachineInstr/MC lowering is not implemented yet. Keep recognition
# buildable while llvm/tools/squall-compiler provides the executable IR path.
add_llvm_target(SquallCodeGen
  SquallTargetMachine.cpp
  LINK_COMPONENTS
  Core
  MC
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
