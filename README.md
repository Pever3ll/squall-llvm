# Squall Compiler

LLVM and Clang with a backend for the Squall v2 explicit-token-store dataflow
processor. `clang --target=squallv2` and `llc -mtriple=squallv2` produce complete
Squall memory images through the standard SelectionDAG, register allocation,
and MC pipeline; `llvm-mc` assembles and disassembles the team assembly syntax.

Build instructions, the code generation design, the assembly syntax, the test
harness, and current limitations are documented in
[llvm/utils/squall](llvm/utils/squall/README.md).
