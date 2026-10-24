# Squall v2 LLVM backend

The `Squall` target (`llvm/lib/Target/Squall`) is an LLVM backend for the Squall
v2 explicit-token-store dataflow processor. `clang --target=squallv2` and `llc`
compile C and LLVM IR into a complete Squall memory image in an ELF object;
`llvm-mc` assembles and disassembles the team assembly syntax.

This directory holds the build helper, a loader and test harness
(`squall_tools`), examples, and the execution tests.

## Build

Prerequisites: CMake 3.20+, Ninja, Python 3.11+, and a C++17 host compiler.

```sh
python3 -m pip install ninja
python3 llvm/utils/squall/build.py --build-dir ../squall-llvm-build --jobs 6 --check
```

The repository import omitted LLVM's libc sources and most of `llvm/test`.
`build.py` restores them from the matching upstream commit
(`llvmorg-23.1.2`, `85ac5602`), checking a source fingerprint first and refusing
to overwrite modified files. It configures with `native-cache.cmake` (Squall as
the only target, Clang enabled, assertions on, shared libraries), builds, and
with `--check` runs the target-parser unit tests, the Squall lit tests, selected
upstream lit suites, and a C-to-object smoke test.

For the LLVM tools alone (as CI does):

```sh
python3 llvm/utils/squall/build.py --build-dir ../build --llvm-only --static-tools \
  --target llc --target llvm-mc --target llvm-objcopy --target llvm-nm
```

## Using the compiler

```sh
clang --target=squallv2 -O2 -c -mllvm -squall-entry=kernel examples/kernel.c -o kernel.o
python3 -m pip install -e 'llvm/utils/squall[test]'
squall-image kernel.o -o out/kernel --arg 7     # loader: write the arguments
squall-run out/kernel --expect 45                # pinned Zig reference interpreter
squall-rtl out/kernel --variant production       # generated Isacomp RTL
```

`llc -mtriple=squallv2` accepts the same input as LLVM IR. `-filetype=asm`
output reassembles with `llvm-mc -filetype=obj` to identical image bytes.

Codegen options:

| Option | Default | Meaning |
| --- | --- | --- |
| `-squall-entry=NAME` | `main` | Function started by the image's start token |
| `-squall-memory-bytes=N` | 524288 | Physical memory size of the image |
| `-squall-stack-tokens=N` | 1024 | Token stack capacity |

With Clang, pass them through `-mllvm`. Entry arguments are not compile-time
options: the loader writes them into argument slots, so one object serves every
input.

## How code generation works

Squall has no register file and no program counter. A token carries a value, a
frame pointer (FP), and an instruction address; each instruction names its
successor and may read a matching slot relative to the FP. The backend uses the
standard LLVM pipeline with that model mapped as follows.

**Registers are frame slots.** `S0`-`S127` are frame words 3-130 of the current
activation (`SquallRegisterInfo.td`). Words 0-2 hold the return tag and an
expansion scratch slot. SelectionDAG instruction selection produces pseudos
such as `ADDrr`, `SETCC`, `LDP` (`SquallInstrInfo.td`); the register allocator,
PHI elimination, spilling, and branch folding all work unchanged. Frame lowering
places frame objects after the highest register a function uses.

**Pseudos expand to token chains.** The AsmPrinter expands each pseudo into
encoded Squall words. A chain's final token continues at the next word, so
machine code runs in layout order with one control token; branches name block
labels. For example, `%r = add i32 %a, %b` becomes: read `S0` into the token,
`add.r` with slot `S1`, sign-extend the 64-bit result to 32 bits, store into the
result register. A read of the register just stored is omitted because the
store's token still carries the value.

**Values.** Integers up to 64 bits and 32-bit pointers. Registers hold 32-bit
values sign-extended to the 64-bit token; 64-bit integers are legalized into
32-bit halves.

**Memory.** Every four-byte logical cell (`i32`, a pointer, half of an `i64`)
occupies one 64-bit matching slot, so a pointer is four times a word address and
C sizes stay conventional. Frame objects, and globals below word 2048, are read
and written as static slots. Other pointers go through an inline helper: the
caller sends a token whose FP is the target word, and the helper accesses frame
slot 0 there and returns through a saved continuation tag. The pointer is checked
for alignment, the reserved low words, and the memory bound first (status 3).
`memcpy`/`memset`/`memmove` with constant sizes are expanded inline.

**Calls.** A callee's frame starts just past the caller's frame. The caller checks
that the new frame fits in the frame arena (status 2 otherwise), writes argument
`I` into the callee's `S<I>`, stores its continuation tag as the callee's return
tag, and sends a token with the FP advanced. Every register survives a call,
because activations never share slots. Up to 16 arguments; `byval` aggregates
are copied into a caller frame object; larger return values use a hidden
pointer.

**Image.** The whole module is one dense memory image in `.squall.image`:

| Words | Contents |
| --- | --- |
| 0 | `__squall_halt`, the discard destination |
| 1, 2 | Result and status (1 success, 2 call depth, 3 bad address) |
| 3-6 | Memory-helper mailboxes |
| 8-15 | Entry argument slots, written by the loader |
| 16+ | Globals |
| 256+ | Runtime stubs (`__squall_start`, host return, error stubs), then functions |
| 1/4 of memory | Frame arena, initialized to constant zero slots |
| top | Token stack, holding the start token |

`.squall.config` holds a JSON description of this layout. All references
resolve inside the section, so objects carry no relocations.

## Assembly syntax

```text
entry:   add.r (0x58).l.r [0x4]  ; dest offset 0x58 words, port L; secondary to the next word, port R
         dupr.r finish.l (data)  ; code-relative slot named by a label
         add finish.l #-7        ; '#' marks a monadic immediate
finish:  write 1
mint:    extract.r out.l callee.r [0]
out:     send.r halt.l [0]
callee:  nop halt.l #0
data:    .slot 42
```

| Spelling | Meaning |
| --- | --- |
| `op`, `op.r`, `op.c` with a slot | Once, Read, Const matching |
| `label.l` / `label.r`, `(N).l` | Destination and its port; `N` is a signed word offset |
| `label.l.r` (or `label.l r`) | Also send the secondary result to the next word, port R |
| `[N]`, `(label)` / `(N)`, `N` | Frame-relative, code-relative, absolute slot |
| `#N` | Signed 32-bit immediate (no matching suffix) |
| `write N` | Store the token value at absolute word `N` |
| `.slot N` / `.word N` | Constant slot / raw 64-bit word |
| `.squall_origin N` | Word address assumed for `.text` (default 256) |

Instruction fields use 64-bit word units. Out-of-range fields, bad suffixes,
and misaligned or out-of-range label references are errors. `examples/assembly_add.sqa`
is a small handwritten program. `llvm-mc` output contains only the assembled
section; complete runnable images come from `llc`/`clang`.

## Tests

```sh
export SQUALL_LLVM_BIN=../squall-llvm-build/bin
export SQUALL_ISACOMP_DIR=/path/to/isacomp   # optional: RTL tests
python3 -m pytest -q llvm/utils/squall/tests
python3 ../squall-llvm-build/bin/llvm-lit -v llvm/test/CodeGen/Squall llvm/test/MC/Squall
```

* `test_codegen.py`: IR at `-O0` and `-O2` executed on the pinned Zig
  interpreter (integer operations and boundaries, comparisons, narrow and 64-bit
  integers, branches, PHIs, loops, recursion, calls, globals, structs, pointer
  memory, guards, assembly round trip).
* `test_c.py`: C examples and seeded random programs at `-O0`/`-O1`/`-O2`,
  compared with the same C compiled and run on the host.
* `test_mc.py`: `llvm-mc` encodings compared with the upstream Zig assembler,
  disassembly round trip, and assembler diagnostics.
* `test_diagnostics.py`: unsupported input produces errors, not crashes.
* `test_rtl.py`: images on the baseline, pipelined, and production (HPDcache)
  RTL, including seeded memory stalls.

`rtl_regression.py` runs the full C matrix on every RTL configuration and writes
a report. The Zig interpreter (pinned commit `03dcc01f`) is fetched and built
automatically; the RTL tests need Icarus Verilog, Verilator, and the Isacomp
checkout pinned in `squall_tools/rtl.py`.

## Limitations

* No byte or 16-bit memory accesses (`char` arrays, strings), floating point,
  atomics, function pointers, varargs, or dynamic `alloca`.
* One closed module per image: calls must reach functions defined in the module,
  and there is no linker or C runtime library.
* Execution is serial: one control token runs at a time. The memory helper's
  shared mailboxes would need redesigning before memory operations can overlap,
  and the compiler does not yet expose Squall's dataflow parallelism.
* Code is not optimized for Squall beyond the read-after-store forwarding above;
  each operation reads its operands from frame slots.
