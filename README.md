# Squall Compiler

The executable compiler path is [llvm/tools/squall-compiler](llvm/tools/squall-compiler/README.md).
It parses and verifies LLVM IR, lowers a defined integer/pointer/array subset to Squall v2
instructions, emits loadable images, and runs those images against the pinned
Zig interpreter and the Isacomp baseline, pipelined, or production RTL. The assembler accepts
the proposed team syntax, and the memory ABI is documented alongside the compiler. Start there for compiler
development and execution tests; building the LLVM monorepo is not required.

For the native LLVM/Clang toolchain, use the pinned source restoration and build
helper in [llvm/utils/squall](llvm/utils/squall/README.md). The complete configured
LLVM 23.1.2 / Clang build works with Squall enabled; its default cache selects
Squall as the only backend.

The native LLVM target remains a recognition/registration scaffold. Its target
triple, data layout, Clang target interface, relocation identifiers, and CMake
source lists have been repaired, but `llc` instruction selection and native MC
assembly/object emission are not implemented. The standalone compiler does not
claim to supply those interfaces.
