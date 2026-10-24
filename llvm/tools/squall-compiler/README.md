# Executable Squall v2 compiler baseline

This tool compiles verified LLVM IR into a labelled Squall assembly listing and
a complete memory image. It provides a conservative baseline for testing compiler
correctness before parallel dataflow scheduling or native `llc` integration.
LLVM parses and verifies the IR through llvmlite 0.50.0 (LLVM 22.1). Unsupported
IR fails with a diagnostic; it is never replaced with a placeholder instruction.

## Install and run

Python 3.11 or newer, Git, and a C runtime are required. From this directory:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -e '.[test]'
.venv/bin/squall-compile examples/add.ll -o out/add --arg 7 --arg 35
.venv/bin/squall-run out/add --expect 42 --trace
.venv/bin/squall-compile examples/fibonacci.ll -o out/fib --arg 8 --frames 16 --stack-tokens 2
.venv/bin/squall-run out/fib --expect 21
```

The runner builds a small driver around the **actual upstream interpreter**,
pinned to `03dcc01ffd68abe5cf6169bd2af6090e0f398077` in
[squall-v2-reference-impl](https://github.com/Lyssia-Seiden/squall-v2-reference-impl).
It requires Zig **0.16.0**, included in the test extra. It fetches the pinned
source on the first run and caches the executable outside the repository. To use
an existing, clean checkout at that revision instead, set `SQUALL_REFERENCE_DIR`.
A different revision or modified interpreter/assembler source is rejected.

The image directory contains:

| File | Purpose |
| --- | --- |
| `program.sqa` | Labelled, one-word-per-record assembly, including local constant pools |
| `memory.hex` | Sparse `$readmemh` image; `@` addresses index **64-bit words** |
| `memory.bin` | Dense, zero-initialized, little-endian image for the interpreter/loader |
| `manifest.json` | ABI, memory bounds, launch state, arguments, function layouts, source hash |
| `symbols.json` | Label-to-word-address map |
| `params.txt` | Existing Isacomp-style launch parameters; stack pointers are **bytes** |

`squall-run` records result bits, signed i32 result, status, cycles, and peak token
usage in `reference-result.json`. `--trace` also writes `reference.trace`.
It returns nonzero for a wrong expected answer, non-quiescent execution, exceeded
call depth, interpreter failure, or token-stack overflow.

With Clang on PATH, an ordinary C frontend can supply the supported IR:

```sh
clang -O0 -S -emit-llvm examples/kernel.c -o out/kernel.ll
.venv/bin/squall-compile out/kernel.ll --entry kernel --arg 7 -o out/kernel
.venv/bin/squall-run out/kernel --expect 45
```

This uses Clang to produce scalar LLVM operations. It does not implement a
general C ABI. Aligned i32/pointer cells and fixed arrays use the restricted
cell-memory ABI below; structs and external library calls remain unsupported.
Higher optimization levels may introduce intrinsics or types outside the subset.

## Supported lowering

| Area | Implemented |
| --- | --- |
| Values | `i1`, `i32`, `ptr`, and `void` returns |
| Arithmetic | `add`, `sub`, `mul`, signed/unsigned division and remainder |
| Bit operations | `and`, `or`, `xor`, `shl`, `lshr`, `ashr` |
| Conditions | All integer `icmp` predicates; `select` |
| Conversions | `trunc`, `zext`, `sext` between supported widths |
| Control flow | Unconditional/conditional branches, loops, parallel PHI edge copies |
| Fanout | Repeated SSA reads from activation-local Constant slots |
| Memory | Entry-block fixed i32/ptr allocas, nested arrays, initialized/zeroed globals, aligned loads/stores, i32-index GEP |
| Memory intrinsics | Constant-length, aligned, nonvolatile `llvm.memcpy` and constant-byte `llvm.memset`; lifetime markers |
| Calls | Direct calls, void calls, bounded recursion, explicit return tags |

Arithmetic normalizes results to the LLVM width. Negative i32 values are sign
extended when stored in a Squall slot. Unsigned division, remainder, and logical
right shifts use zero-extended inputs. Signed ordering is implemented with sign
bit transformation and Squall's subtraction borrow output. LLVM's defined input
requirements still apply: division by zero, out-of-range shifts, poison, and
overflow under `nsw`/`nuw` are not assigned new language semantics here.

One control token sequences source instructions and memory effects. A branch
activates one edge. PHI lowering saves **all** incoming values before committing
any PHI destination, including loop-carried swaps. A comparison's secondary
result is captured and drained before its primary continuation runs; long loops
therefore do not accumulate pending halt tokens. This is deliberately serial
and does not yet exploit Squall's available execution parallelism.

Rejected features include other integer widths, floating point, vectors,
byte/unaligned memory, structs, dynamic allocation, non-entry allocas,
external/indirect calls, varargs, unsupported intrinsics, exceptions, atomics, and
volatile accesses. GEP indices must be i32. Host pointer entry arguments and
non-null pointer global initializers are rejected; use a wrapper to pass local
or global arrays to internal functions. Pointer constants expressed as constant
GEP expressions, pointer/integer casts, and general address spaces are unsupported. There is no linker or runtime library. Every definition in the module
must be in the subset, including definitions unreachable from the entry.

## Activation and image ABI

The current ABI is `squall-cell-memory-2`; changes to this layout require a
new version. Older scalar images retain their own version in their manifest. It is a compiler/runtime ABI, not an existing platform C ABI.

* The token accumulator carries an i1 boolean, a sign-normalized i32, or a
  zero-extended logical pointer. Slots
  contain 62 payload bits and Constant presence bits; LLVM i32 remains 32 bits.
* FP and IP inside tags are **word addresses**. A packed tag is
  `ip | (fp << 29) | (port << 58)`. A token's first physical word is `tag << 2`,
  followed by its 64-bit accumulator.
* Frame slot 0 contains the packed return tag. Arguments follow in declared
  order, then SSA values, aligned local i32/pointer cells and arrays, and scratch/PHI slots. The manifest
  records each function's argument offsets and required size.
* All activations use a common power-of-two stride, at most 1024 words. A call
  checks the frame limit **before** writing the callee's arguments and return
  tag. Calls use the next frame; returns use `Send` through frame slot 0.
* Host arguments are real initial tokens. Once joins ensure all argument stores
  complete before entry, even when argument tokens arrive in a different order.
* Word 1 holds the returned value. Word 2 is initially 0, becomes 1 on successful
  completion, 2 on call-depth exhaustion, or 3 on a rejected dynamic memory address. The result write precedes success.
  The runners also require quiescence; a hardware host must synchronize with
  its memory system before reading results.
* The initial token stack contains the entry argument tokens, or one launch
  token for a zero-argument entry. At least `max(2, argument_count)` token slots
  are required. Once entry joins finish, this serial lowering uses at most two
  live tokens, including comparisons and calls; tests exercise tight bounds.
* Default memory capacity is 512 KiB. Scalar globals start at word 16, code at word 256, global arrays at one
  eighth of memory capacity, and the frame arena at one quarter (with alignment
  padding). When arrays are present, code must end before their arena. The token
  stack grows downward from `memory_bytes - 16`. Layout checks reject region
  overlap, exhausted address fields, and insufficient reserved capacity.
* Code and constant pools remain immutable during execution. Reload the complete
  image to start a new invocation; bootstrap slots and frames are not a persistent
  multi-launch interface. A physical loader must zero unspecified locations if
  it uses the sparse hex instead of the complete binary.

## Restricted pointer and array memory

A logical LLVM i32 or pointer cell occupies four logical bytes and one physical
64-bit Squall matching slot. For a live cell at physical word `W`, its pointer
value is `4 * W`. GEP uses ordinary logical byte offsets; an i32 element adds
four. The physical SRAM byte address is twice the logical pointer. This preserves
32-bit C integer/pointer sizes without attempting to pack data into Squall's
presence bits. Allocation alignment is honored in logical pointer units, including
padding and a uniform activation stride. The manifest records the mapping.

This is an explicit compiler/runtime representation, **not a flat byte-addressed
platform C ABI**. A physical host must marshal data with this mapping. There is
no byte aliasing, heap, general struct layout, or automatic host-pointer import.

Static scalar accesses retain direct slot instructions. Dynamic accesses save a
continuation, construct a packed tag with FP equal to the addressed physical word,
and `send` to a helper using `[0]`. The helper returns with the original caller FP.
Mailboxes at physical words 3 through 6 contain the continuation, store value,
helper IP, and checked pointer. They are safe only under this serial control-token
ABI: helpers cannot overlap or reenter. No processor RTL changes are required.
Parallel scheduling must replace this mailbox protocol before overlapping accesses.

Dynamic accesses check four-byte alignment, exclude reserved words below 16, and
check the physical memory bound before rebasing FP. These checks do not enforce
object provenance, lifetime, or per-array bounds; LLVM's defined-behavior requirements
still apply. Stores to code, expired frames, or past the end of an object are not
supported operations. `inbounds` GEP is not permission to access outside an object.
Fixed allocas are restricted to the entry block so repeated allocation cannot
silently reuse storage that should remain simultaneously live.

`llvm.memcpy`/`llvm.memset` require explicit alignment of at least four bytes,
nonvolatile operation, and constant length divisible by four, at most 1024 bytes.
Memcpy preserves each cell's value; memset expands a constant byte to each i32
cell. Overlapping memcpy has LLVM's usual undefined behavior. Lifetime markers
have no effect in this fixed frame model. Larger copies can also exceed code
arena limits and fail with a diagnostic.

For example, using the built Squall frontend:

```sh
/path/to/squall-llvm-build/bin/clang --target=squallv2-unknown-none -O0 \
  -S -emit-llvm examples/memory_kernels.c -o out/memory.ll
squall-compile out/memory.ll --entry dot --arg 8 -o out/dot
squall-run out/dot --expect 163
squall-rtl out/dot --variant production --memory-latency 5
```

`memory_kernels.c` also contains a reduction, local array passed across a call,
initialized local array using memcpy, and a small nested-array matrix product.
The host C library, reference interpreter, and RTL independently check the answers.

## Assembler

```sh
squall-asm out/add/program.sqa -o out/add/code-only.hex
```

This produces **code/data records only**. Use `squall-compile`'s `memory.hex` or
`memory.bin` to include frames, host storage, bootstrap slots, and launch tokens.
The compiler prints the team syntax. Legacy labelled `key=value` input remains
accepted for existing programs, with identical machine encoding.

```text
entry:   add.r (0x58).l.r [0x4] ; relative instruction destination, frame slot
next:    write 3               ; receives the secondary overflow token
read:    dupr.r finish.l (data) ; code-relative slot named by a label
imm:     add finish.l #-7      ; '#' makes immediates unambiguous
finish:  write 1
mint:    extract.r output.l callee.r [0]
output:  send.r halt.l [0]
callee:  nop halt.l #0
data:    .slot 42
```

| Spelling | Meaning |
| --- | --- |
| `add`, `add.r`, `add.c` with a slot operand | Once, Read, Const matching |
| `label.l` / `label.r` | Primary instruction destination, resolved relative to this word |
| `(N).l` / `(N).r` | Literal signed instruction offset in words |
| `label.l.r` | Primary port L, secondary port R at the immediately following word |
| `label.l r` | Accepted input alias for the separated secondary port |
| `[N]` | Unsigned 11-bit frame-relative word offset |
| `(label)` / `(N)` | Label-based / literal signed 11-bit code-relative slot offset |
| `N` | Absolute unsigned 11-bit slot word address |
| `#N` | Signed 32-bit monadic immediate, without a matching suffix |
| `write N` | Absolute unsigned 29-bit word address |
| `extract.r out.l target.r [N]` | Emit a packed target tag to `out`, preserving current FP |
| `send.r local.l [N]` | Remote destination is the packed left operand; local copy goes to `local` |
| `.slot N` / `.word N` | Constant matching slot / raw 64-bit data word |

Ports describe outgoing token destinations. They do not set the incoming token
port, which is already in its tag. The secondary port is omitted when S=0; when
present it sets S=1 and gates the opcode's second result. It does not encode an
independent secondary instruction offset. Send and Extract do not have this ALU
secondary-token field. A Send itself emits a remote token and a local copy.

Comments start with `;`. Old `#` comments remain accepted on legacy records.
Labels can stand alone or precede a record; unlabelled team-syntax records receive
internal labels. Labels cannot end in a port suffix `.l` or `.r`, which would be
ambiguous with the destination grammar. `halt` is reserved for word zero.

All addresses and offsets in this assembly are **64-bit word units**, including
Write, frame slots, absolute slots, and literal instruction offsets. The parser
checks duplicate/undefined labels, operand counts, matching suffixes, ranges,
secondary adjacency, opcodes, and data widths without silently truncating fields.
Tests compare legacy and team spellings with the actual upstream Zig assembler's
semantic API and execute `examples/assembly_add.sqa` on its interpreter.

## Tests and RTL differential execution

```sh
.venv/bin/python -m pytest -q
```

The tests execute emitted machine words with the independent Zig reference,
cover integer boundaries and every implemented arithmetic/comparison operation,
fanout, PHIs, loop swaps, recursion, depth errors, memory sequencing, deterministic
images, assembler diagnostics, CLI failures, and completion ordering. A separate
encoding probe compares all ALU opcode encodings and instruction forms with the
upstream Zig assembler. The C frontend test compares Squall with native execution
of the same C function when Clang is available.

For baseline RTL integration, install Icarus Verilog and point to a clean
Isacomp checkout at **899ee4456cabd88deb7d2ab1088ad721178dfb8b** (the reviewed
`squall-v2-hpdcache` revision):

```sh
export SQUALL_ISACOMP_DIR=/absolute/path/to/isacomp-checkout
.venv/bin/python -m pytest -q tests/test_rtl.py
.venv/bin/squall-rtl out/fib
```

The default runner generates and compiles the **single-cycle baseline** from that
revision. Two additional variants use the full six-stage pipeline and the actual
production generator (native instruction cache, write-back HPDcache, sliding
stack cache, and shared external memory):

```sh
.venv/bin/squall-rtl out/fib --variant pipeline
.venv/bin/squall-rtl out/fib --variant production --memory-latency 5
```

Seeded response-delay tests reuse the production simulator at runtime:

```sh
squall-rtl out/dot --variant production --memory-latency 5 \
  --stall-seed 17 --stall-jitter 19
```

Each external-memory read/write port uses its own deterministic pseudo-random
state. Requests receive delays from base latency through base latency plus jitter;
read data remains poisoned until ready, responses remain held, address changes
restart an access, and upstream write ordering remains intact. A seed reproduces
timing; different seeds are tested to change cycle counts while preserving results.
This exercises response backpressure, not arbitrary on-chip protocol faults.

Production simulation requires **Verilator** and the pinned HPDcache submodule.
Initialize that submodule with `git submodule update --init vendor/cv-hpdcache`
in the Isacomp checkout. The runner verifies the Isacomp and HPDcache revisions,
generates fresh RTL, and caches the compiled simulator. Build provenance records
the simulator version, source revision, compilation command, and RTL/bench hashes.

Every RTL run checks successful drain, stack bounds, absence of simulator errors,
and completion of cache flush and write quiescence before reading memory. It
compares the result, status, consumed useful tokens, and final memory with the
independent Zig execution. The baseline must match token order exactly. Pipeline
and production may reorder only the bootstrap argument tokens, whose multisets
must match; after entry they must match the serial ABI's useful-token order exactly.
The reports also record whether the entire raw useful-token order matched.
Only reserved token-stack scratch and the undefined payloads of spent bootstrap
Once slots are excluded from final-memory comparison. Code, globals, and all
activation frame storage are checked.

Baseline runs write `rtl.trace` and `rtl-result.json`. Other variants include the
variant and latency in their output filenames. Failures return nonzero and retain
traces; a result alone is insufficient to pass. The harness accepts images up to
512 KiB. RTL pytest tests explicitly skip when `SQUALL_ISACOMP_DIR` is absent;
reference execution tests still run. Negative tests inject a wrong argument and
corrupt an unused frame word to check that the runner detects mismatches even
when the returned value is correct.

For the native Clang-to-RTL regression, from the repository root:

```sh
python llvm/utils/squall/test_rtl.py \
  --clang ../squall-llvm-build/bin/clang \
  --isacomp-dir /absolute/path/to/isacomp-checkout \
  --reference-dir /absolute/path/to/squall-v2-reference-impl \
  --output ../squall-memory-regression \
  --include-memory --random-seeds 0 1 7 42 113 2026 --stall-seeds 17 91
```

The base regression has 40 C input cases and five explicit loop/parallel-PHI
cases. The options above add 13 memory benchmark inputs and 18 inputs from six
seeded C programs: **76 cases across seven configurations (532 RTL runs)**. These
include baseline, full pipeline, production latency 0/5/11, and two production
response-jitter seeds. The generated C avoids signed overflow, zero divisors,
out-of-range shifts, uninitialized reads, and invalid array indices. The C inputs are compiled by the native Squall
Clang; expected answers come from a separately compiled host C library. Cases
cover signed/unsigned boundaries, division/remainder, shifts, comparisons, fanout,
branches, loops, globals/local storage, void calls, four-argument calls, and
recursion. The report, generated sources, LLVM IR, code-size/token/cycle metrics, simulator
provenance, and per-image reference/RTL traces remain under `--output`. CI runs
reference and RTL jobs; its lightweight upstream Clang i386 target supplies
32-bit-pointer IR. Local validation additionally uses the built Squall Clang.
The workflow has been added locally; its GitHub-hosted execution is unverified.

These directed simulation checks do not establish tapeout signoff. Broader
randomized concurrent protocol tests, self-modifying-code/coherence behavior, formal
properties, and physical implementation checks remain separate work.

## Native LLVM path

The native C++ target now implements `llvm-mc` parsing/encoding, instruction
printing, disassembly, ELF fixups/relocations, and `llc` LLVM IR-to-token lowering.
The built Clang can emit Squall ELF objects directly. This Python compiler and
assembler remain independent implementations for differential testing; native
code generation does not invoke them or a Python subprocess.

The native target lowers IR directly to MCInsts through the legacy code-generation
pass manager, using the same serial cell-memory ABI and restricted source subset.
A conventional register-based MachineInstr pipeline is not used. See the
[native build and usage guide](../../utils/squall/README.md).

`squall-image input.o -o out/image --objcopy /path/to/llvm-objcopy` unpacks the
native object's `.squall.image` and `.squall.config` sections into the existing
runner format. This command only packages already encoded bytes; it performs
no instruction selection, assembly, relocation application, or code generation.

General byte/struct/heap access, separate compilation and linking, parallel
scheduling, MIR and JIT remain outside the implemented scope. Native ELF machine
ID 999 and relocation assignments are experimental project conventions.

## GitHub Actions access

The reference and native jobs run automatically and fetch the public pinned Zig
reference. The native job builds LLVM MC/llc tools and runs LLVM lit plus native
encoding/execution differential tests; it uses upstream Clang for C-to-IR tests.
The RTL job needs access to the private `Jbalkind/isacomp` repository. Configure a
repository secret `ISACOMP_READ_TOKEN` with read-only contents access to that
repository, then set the repository variable `SQUALL_RTL_ENABLED` to `true`.
Without that variable the RTL job is skipped; a green reference job alone does
not establish RTL coverage. Fork pull requests cannot run the privileged RTL job;
review the changes before running them on a trusted branch. The RTL job uses
upstream Clang's i386 target for 32-bit-pointer IR. Building Squall Clang remains
a separate local validation step.
