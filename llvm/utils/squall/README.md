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

## Native assembler and compiler

The native target supports the documented i1/i32/pointer/fixed-array subset. Its
C++ lowering pass converts LLVM IR into Squall token/slot operations, then emits
MCInsts through LLVM's MCStreamer. No Python compiler or assembler runs in this
path. It uses the legacy `llc` code-generation pass manager; it does not implement
a register allocator, a MachineInstr/MIR pipeline, new-PM codegen, or JIT.

From the repository root, with the Python package installed for image unpacking
and simulation:

```sh
python3 -m pip install -e './llvm/tools/squall-compiler[test]'
../squall-llvm-build/bin/clang --target=squallv2-unknown-none -O0 -S -emit-llvm \
  llvm/tools/squall-compiler/examples/kernel.c -o kernel.ll
../squall-llvm-build/bin/llc -mtriple=squallv2-unknown-none -filetype=obj \
  -squall-entry=kernel -squall-arg=7 kernel.ll -o kernel.o
squall-image kernel.o -o out/kernel --objcopy ../squall-llvm-build/bin/llvm-objcopy
squall-run out/kernel --expect 45
```

For direct native frontend-to-object generation, Clang accepts the launch options
through `-mllvm`:

```sh
../squall-llvm-build/bin/clang --target=squallv2 -O0 -c \
  -mllvm -squall-entry=kernel -mllvm -squall-arg=7 \
  llvm/tools/squall-compiler/examples/kernel.c -o kernel.o
```

Omit `-filetype=obj` from `llc` for textual assembly. Reassemble that output with
`llvm-mc -triple=squallv2 -filetype=obj kernel.s -o kernel.o`. The resulting image
bytes must match direct object emission.

### Handwritten assembly and objects

```sh
../squall-llvm-build/bin/llvm-mc -triple=squallv2 -filetype=obj program.sqa -o program.o
../squall-llvm-build/bin/llvm-objdump -d program.o
../squall-llvm-build/bin/llvm-readobj -r program.o
```

The parser accepts the team ALU/send/extract/write forms, `.slot` and 64-bit
`.word`, plus normal LLVM directives such as `.section`, `.quad` and `.zero`.
Instruction fields and literal destination offsets use **64-bit word units**;
ELF symbol values and symbolic expression addends use **byte units**. Local
label fixups check eight-byte alignment and signed field ranges before scaling.
Raw `.text` defaults to physical word 256 for the predefined `halt` destination;
`.squall_origin N` changes that assumed word origin. This directive does not add
padding or link/place a section. A raw assembly object holds code/data sections;
it needs host launch state before it is a complete executable image.

The native compiler's objects instead contain a dense, zero-initialized
`.squall.image` section beginning at physical address zero, and a JSON
`.squall.config` section containing the ABI, launch state, symbols and memory
layout. `squall-image` extracts those bytes with `llvm-objcopy`. It does not compile
or re-encode instructions. Both reference and RTL runners consume this image.

ELF32 machine ID 999 is an experimental Squall assignment already used by this
repository. Relocations are `R_SQUALL_32`, `R_SQUALL_64`, `R_SQUALL_BRANCH20`,
`R_SQUALL_TARGET20` and `R_SQUALL_CODE11`. The three instruction relocations apply
`(S + A - P) / 8` to their respective signed fields. ELF relocation addends are
bytes. Object emission and relocation recording are implemented; there is no
Squall linker or external-symbol resolver. Compiled images resolve all internal
references before emission and require all called functions in the input module.

The ABI remains restricted and serial: no byte/struct/heap memory, external calls,
flat host-pointer imports or overlapping memory helpers. Native code generation
uses `-squall-entry` (default `main`), repeated `-squall-arg`, `-squall-memory-bytes`,
`-squall-frames`, and `-squall-stack-tokens`. These build a complete launch image,
not a general platform C ABI. The Python implementation remains an independent
reference. See [native validation](NATIVE-VALIDATION.md).

For the smaller MC/llc-only CI build, `build.py --llvm-only --static-tools` omits
Clang and links only requested tool dependencies. This is separate from the
complete configured LLVM/Clang build above. `--check` requires Clang.
