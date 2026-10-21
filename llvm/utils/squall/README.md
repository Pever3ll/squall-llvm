# Native LLVM/Clang build for Squall

This builds the LLVM libraries, LLVM command-line tools, Clang frontend and tools,
and the Squall target registration from this checkout. The default cache selects
**Squall only**. Other production architectures, runtimes, examples, benchmarks,
and LLVM language bindings are outside that configuration.

The source import corresponds to LLVM **23.1.2**, but originally omitted all libc
implementation files and the LLVM lit test configuration. `build.py` can restore
the missing files from upstream commit
`85ac560262434c9ccfc0c183ec22d4138ed647fb` (`llvmorg-23.1.2`). It verifies the
`APFloat.cpp` source fingerprint before restoring and refuses to overwrite changed
libc files. The restored libc sources are unmodified upstream files with their
upstream license. Provenance is recorded in `libc/UPSTREAM-SOURCE.json`.

## Build

Prerequisites: CMake 3.20 or newer, Ninja, Python 3.11 or newer, and a supported
C/C++17 host compiler. On macOS, install Xcode Command Line Tools and CMake.
Use a terminal where the desired Python and Ninja are on `PATH`:

```sh
python3 -m pip install ninja
python3 llvm/utils/squall/build.py --build-dir ../squall-llvm-build --jobs 6 --check
```

The helper restores any missing pinned sources, configures with
`native-cache.cmake`, runs the **default complete build target**, builds the target
parser unit test, and checks target registration, Clang preprocessing, C-to-LLVM
emission, bitcode assembly, and verification. A failed command stops the helper.
Builds are incremental; rerun the same command after editing the source.
`--configure-only` stops before compilation. `--target clang` or other repeated
target options can narrow subsequent development builds.

The cache uses MinSizeRel, shared LLVM/Clang libraries, assertions, no debug
symbols, no precompiled headers, and one concurrent link. These options reduce
disk usage; they do not skip the native compilation or linking steps. More disk
space is required for additional architecture backends or debug information.

To configure directly or change target selection:

```sh
cmake -S llvm -B ../squall-llvm-build -G Ninja \
  -C llvm/utils/squall/native-cache.cmake
cmake --build ../squall-llvm-build -j 6
cmake --build ../squall-llvm-build --target TargetParserTests -j 6
../squall-llvm-build/unittests/TargetParser/TargetParserTests
../squall-llvm-build/bin/llvm-lit -v clang/test/Preprocessor/squall.c clang/test/CodeGen/squall.c
```

For a host backend, pass `-DLLVM_TARGETS_TO_BUILD=AArch64` on an ARM Mac or
`-DLLVM_TARGETS_TO_BUILD=X86` on an x86 machine. This increases build time and disk
usage. The default Squall-only build cannot generate host machine code or JIT
execute host programs.

The original import omitted LLVM's regression test inputs. The helper restores
the assembler, verifier, TableGen, and FileCheck suites and the test infrastructure,
not the entire upstream LLVM regression corpus. `--check` runs those suites as
well as the native Squall tests. The existing LLVM unit tests and Clang tests are present.
Do not report `check-llvm` as the complete upstream regression suite unless the
matching full `llvm/test` corpus has also been restored. Focused Squall lit tests
and the linked target parser unit suite provide the native target checks here.

## Compiler scope

A successful native build makes the LLVM tools and Clang available. Squall
MachineInstr instruction selection, native MC encoding, and ELF object generation
remain unimplemented. Use Clang's LLVM IR output as input to the executable
`llvm/tools/squall-compiler` path:

```sh
../squall-llvm-build/bin/clang --target=squallv2-unknown-none -O0 -S -emit-llvm \
  llvm/tools/squall-compiler/examples/kernel.c -o kernel.ll
squall-compile kernel.ll --entry kernel --arg 7 -o out/kernel
squall-run out/kernel --expect 45
```

Native `llc` Squall machine-code emission is still an unsupported operation.
