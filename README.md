# Squall Compiler

Squall v2 now has a native LLVM MC assembler/disassembler and an LLVM IR-to-token
backend. `llvm-mc` assembles the proposed team syntax; `llc` emits Squall assembly
or ELF images; the built Clang can emit Squall objects directly. Build and usage
instructions are in [llvm/utils/squall](llvm/utils/squall/README.md).

The native backend supports the documented integer/pointer/fixed-array subset
using the serial `squall-cell-memory-2` ABI. It lowers IR directly to MCInsts,
without a register allocator or an external compiler process. It emits a complete
image for one entry function and known launch arguments; general C interoperability,
separate compilation/linking, parallel scheduling, MIR and JIT remain future work.

The independent [Python compiler and assembler](llvm/tools/squall-compiler/README.md)
remain available as a reference implementation and lightweight development path.
Their runners execute either implementation's images against the pinned Zig
interpreter and generated baseline, pipelined and production Isacomp RTL.

See [native integration validation](llvm/utils/squall/NATIVE-VALIDATION.md) for
reproducible checks and their scope. The default LLVM build selects Squall only.
