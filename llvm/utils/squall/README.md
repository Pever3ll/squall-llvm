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

Programs of several files are linked before code generation, because an image
is a complete program:

```sh
clang --target=squallv2 -O2 -mllvm -squall-entry=run \
  examples/linked/main.c examples/linked/inventory.c examples/linked/scoring.c -o program.o
# or separately:
clang --target=squallv2 -O2 -c -flto examples/linked/main.c -o main.o   # LLVM bitcode
clang --target=squallv2 -O2 -mllvm -squall-entry=run main.o inventory.o scoring.o -o program.o
```

When the driver links, it compiles each file to bitcode and runs `llvm-lto`,
which merges the modules, optimizes across them with only the entry exported,
and generates the image. `-mllvm` options take effect at that link. An object
from a plain `clang -c` is already a finished one-module image and cannot be
linked further.

The link also includes the Squall runtime, bitcode built by `build.py` into
`lib/clang-runtimes/squallv2` beside the compiler (`--sysroot` selects
another): `libc.bc`, a small C library (`string.h`, `stdlib.h` without a heap,
`ctype.h`, `assert.h`, `errno.h`, and the exact functions of `math.h`), and
`libclang_rt.builtins.bc`, the compiler-rt soft-float builtins. Its headers
are the only system headers; the host's are never searched. `-nostdlib`
omits both libraries and `-nolibc` the C library. Only what the program uses
reaches the image, so a program that uses neither is unchanged. A single
`clang -c` module has no runtime, so floating point and library calls need
the link.

Codegen options:

| Option | Default | Meaning |
| --- | --- | --- |
| `-squall-entry=NAME` | `main` | Function started by the image's start token |
| `-squall-memory-bytes=N` | 524288 | Physical memory size of the image |
| `-squall-stack-tokens=N` | 1024 | Token stack capacity |
| `-squall-fork-join` | off | Run independent work in a block as two tokens |
| `-squall-alloca-bytes=N` | 4096 | Area for variable-length arrays and `alloca` |

With Clang, pass them through `-mllvm`. Entry arguments are not compile-time
options: the loader writes them into argument slots, so one object serves every
input.

## How code generation works

Squall has no register file and no program counter. A token carries a value, a
frame pointer (FP), and an instruction address; each instruction names its
successor and may read a matching slot relative to the FP. The backend uses the
standard LLVM pipeline with that model mapped as follows.

**Registers are frame slots.** `S0`-`S127` are frame words 3-130 of the current
activation (`SquallRegisterInfo.td`). Word 0 holds the return tag, and words 1
and 2 are scratch slots for expansions (word 2 for a second parallel lane). SelectionDAG instruction selection produces pseudos
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

**Values.** Integers up to 64 bits, 32-bit pointers, and `float` and `double`
in software. Registers hold 32-bit values sign-extended to the 64-bit token;
64-bit integers are legalized into 32-bit halves. 64-bit division is expanded
into loops in IR, and `TargetLibraryInfo` marks every C library function
unavailable, so optimizations never introduce calls such as `strlen`.

**Floating point.** `SquallLowerFloat` rewrites float and double arithmetic,
comparisons, and conversions into calls to the compiler-rt builtins
(`__addsf3`, `__ltdf2`, `__fixsfsi`, ...): IEEE single and double precision,
round to nearest even, no exceptions. `fneg`, `fabs`, `copysign`, and
`is.fpclass` become bit operations, `fmuladd` a separately rounded multiply and
add, and `sqrt`, `floor`, and similar intrinsics C library calls. Values that
are only loaded, stored, selected, or passed are softened to integers by
SelectionDAG. Code generation calls runtime functions the program's IR may
not mention, so the runtime lists all of them in `llvm.compiler.used` and LTO
keeps them; once the calls exist, the pass deletes the unused ones with the
variables only they used, as a linker's dead stripping would.

**Traps.** `__builtin_trap`, `abort`, and a failed `assert` halt with status 4.

**Memory.** Memory is byte-addressed in four-byte little-endian cells, one per
64-bit matching slot, so a byte address is four times a word address plus the
byte's position, and C sizes stay conventional. A cell holds its 32-bit value
sign-extended. Frame objects, and globals below word 2048, start on cell
boundaries and are read and written as static slots. Other pointers go through
an inline helper: the caller sends a token whose FP is the target word, and the
helper accesses frame slot 0 there and returns through a saved continuation
tag. The pointer is checked for alignment, the reserved low words, and the
memory bound first (status 3).

Narrower accesses are built from cell accesses. A `char` or `short` load loads
its cell and extracts the bytes during instruction selection. A store that
covers less than a whole aligned cell is rewritten in IR by `SquallLowerMemory`
into a cell update (load, merge, store): SelectionDAG may put stores to
different bytes of one cell on independent chains because they do not alias,
and two updates of one cell must not interleave. As whole-cell accesses to the
same address they stay ordered, and `allowsMemoryAccess` keeps DAG combines
from narrowing them again. Misaligned and packed accesses are split into
bytes. Memory intrinsics on whole aligned cells are expanded inline; others
become loops.

**Calls.** A callee's frame starts just past the caller's frame. The caller checks
that the new frame fits in the frame arena (status 2 otherwise), writes argument
`I` into the callee's `S<I>`, stores its continuation tag as the callee's return
tag, and sends a token with the FP advanced. Every register survives a call,
because activations never share slots. Up to 16 arguments; `byval` aggregates
are copied a cell at a time into a caller frame object; larger return values
use a hidden pointer.

**Variadic functions.** Unnamed arguments travel in the argument registers like
named ones (16 in all). Registers are frame words, so `va_start` stores the
address of the first unnamed argument's register, and `va_arg` reads through
it: four bytes for `int` and pointers, eight for `long long` and `double`,
without realignment. A variadic function reserves `S0`-`S15` so that the
register allocator never overwrites its arguments.

**Dynamic allocation.** Frames have a fixed size, so `SquallLowerAlloca`
allocates variable-length arrays and `alloca` from an area it adds to the
image (`-squall-alloca-bytes`), with a bump pointer. A function that allocates
saves the pointer on entry and restores it before returning;
`llvm.stacksave`/`stackrestore` read and write it. Exhausting the area traps.

**Function pointers.** A token tag packs the instruction word (bits 0-28), the
frame pointer (bits 29-57), and the port (bit 58). A function pointer is the
entry's word address, a tag with frame pointer 0: taking an address mints a tag
for the entry and keeps its instruction field, and an indirect call ORs in the
callee's frame pointer and sends. Initializers hold function addresses as
layout-time expressions, `(entry - halt) / 8`.

**Parallel lanes (`-squall-fork-join`).** Expanded code normally runs with one
control token. `SquallForkJoin` runs after register allocation and before the
frame layout is fixed. Within each block, between terminators, it renames
temporaries that live only inside the region to unused registers, removing
the false dependencies register allocation creates, then groups instructions
that conflict: they share a register that one defines, or both access memory
and one stores to what the other may access. Two balanced groups become
lanes:

```text
fork:    dupl laneB.l #0, secondary token to lane A at the next word
lane A:  ...; dupl join.l #0
lane B:  ...; falls into the join on the right port
join:    dupl (Once) [7]     the first token fills the empty slot and stops;
                             the second empties it and continues
<instructions that combine both lanes>
```

Each lane has its own scratch word. Memory accesses may be in both lanes
when they cannot alias, but accesses through computed pointers and to globals
use the memory helper's shared mailboxes, so they stay in one lane; frame
objects are static slots. A call may run in a lane when it is direct and the
callee cannot reach the caller again; it counts as touching all memory, so
the other lane does only register work. A lane that calls joins on its own
reserved word (absolute words 16-47, before the globals, when fork/join is
on), because the callee may fork and join itself on the shared word 7 while
this join is pending. A join word must start empty: frame words start as
constants, which no match can empty, so joins cannot be frame-relative, and
recursion, which could re-enter a pending region, is excluded. Lanes do not
span blocks, so loops are not overlapped with each other.

A guard that fails in a lane still halts with its status; the other lane
stops at the join. Manifests record `parallel_lanes` and `join_words`, and
simulators should compare executed tokens as a multiset because cores may
interleave the lanes differently.

The pass chooses where the join goes. An instruction that combines two groups
(a merge point) would tie them into one lane, so the pass tries deferring the
last few merge points, with everything that depends on them, to after the
join, and keeps the split with the most balanced lanes (a call weighs eight
instructions). Deferring every merge point would pull whole chains out of the
lanes because of small shared computations such as an address.

On the pipelined and production RTL, in `examples/parallel.c`, `chains` (two
independent computations) runs 1.48x and 1.37x faster, `call_beside_arithmetic`
1.28x and 1.26x, and `two_frame_arrays` 1.56x and 1.33x; all the C examples
together run 1.12x and 1.10x faster. Forking executes extra tokens (about 10%
in `chains`, 2% across the examples), so a core that runs one token at a time
is slower, and small regions can lose on the production core; the pass is off
by default.

**Image.** The whole module is one dense memory image in `.squall.image`:

| Words | Contents |
| --- | --- |
| 0 | `__squall_halt`, the discard destination |
| 1, 2 | Result and status (1 success, 2 call depth, 3 bad address, 4 trap) |
| 3-6 | Memory-helper mailboxes |
| 7 | Join slot of parallel lanes, empty between joins |
| 8-15 | Entry argument slots, written by the loader |
| 16-47 | With `-squall-fork-join`: call join words, empty |
| 16+ or 48+ | Globals, then the alloca area if the program uses one |
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

* `test_runtime.py`: linked programs using the C library, floating point
  (bit for bit with the host, including seeded random operands with
  subnormals, infinities, and NaN), variadic functions, variable-length arrays
  and `alloca`, and traps, compared with the same sources built for the host.
* `test_codegen.py`: IR at `-O0` and `-O2` executed on the pinned Zig
  interpreter (integer operations and boundaries, comparisons, narrow and 64-bit
  integers including division, branches, PHIs, loops, recursion, calls,
  globals, structs, pointer memory, guards, assembly round trip, and every
  width, byte offset, and alignment of narrow memory through static and dynamic
  pointers).
* `test_c.py`: C examples (`bytes.c`, `callbacks.c`, `parallel.c`, and
  others), seeded random programs with byte and 16-bit arrays, and the
  three-file `examples/linked` program, at `-O0`/`-O1`/`-O2` with and without
  fork/join, compared with the same C compiled and run on the host.
* `test_mc.py`: `llvm-mc` encodings compared with the upstream Zig assembler,
  disassembly round trip, and assembler diagnostics.
* `test_diagnostics.py`: unsupported input produces errors, not crashes.
* `test_rtl.py`: images on the baseline, pipelined, and production (HPDcache)
  RTL, including seeded memory stalls and fork/join lanes, which must also run
  in fewer cycles on the overlapping cores.

`rtl_regression.py` runs the full C matrix on every RTL configuration and writes
a report; `--fork-join` compiles it with parallel lanes. The Zig interpreter (pinned commit `03dcc01f`) is fetched and built
automatically; the RTL tests need Icarus Verilog, Verilator, and the Isacomp
checkout pinned in `squall_tools/rtl.py`.

## Limitations

* No atomics, `long double` beyond `double`, `setjmp`, heap (`malloc`), or
  I/O; the C library is a freestanding subset, and `math.h` has only the
  exact functions (no `sin`, `exp`, or `pow`).
* Floating point raises no exceptions and does not honor rounding modes.
* One image per program, linked from bitcode; there is no object-level linker,
  so finished `clang -c` images cannot be combined.
* Code runs with one control token unless `-squall-fork-join` is given; lanes
  stay within a basic block, and recursive or indirect calls end a region.
* Byte stores are read-modify-write updates of their cell, so they cost a
  cell load as well as the store.
