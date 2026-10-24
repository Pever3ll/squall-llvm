# Compiler-to-Squall RTL validation — 2026-10-20

Historical report for the standalone compiler/scaffolding revision. Native LLVM
integration added later is described in [NATIVE-VALIDATION.md](NATIVE-VALIDATION.md).

This records the earlier scalar baseline. The expanded syntax/array/pointer
regression and seeded memory timing results are in [MEMORY-VALIDATION.md](MEMORY-VALIDATION.md).

**225/225 RTL simulations passed**, covering 45 input cases on five configurations.
No processor RTL or Isacomp optimization passes were modified for these runs.

| Configuration | Passed | Useful tokens, summed | Core cycles, summed |
| --- | ---: | ---: | ---: |
| baseline-lat0 | 45/45 | 31,927 | 33,018 |
| pipeline-lat0 | 45/45 | 31,927 | 188,443 |
| production-lat0 | 45/45 | 31,927 | 1,779,867 |
| production-lat5 | 45/45 | 31,927 | 2,617,410 |
| production-lat11 | 45/45 | 31,927 | 3,449,430 |

The 45 cases comprise **40 C inputs** and **five explicit parallel-PHI loop inputs**.
The C source was compiled by the newly built native LLVM 23.1.2 Squall Clang,
then by the executable LLVM-IR-to-Squall compiler and checked image emitter.
Expected C results came from the same source compiled separately with the host
C compiler and executed natively. Every image also ran on the independent,
pinned Zig reference interpreter before RTL comparison.

Coverage includes integer boundaries, signed and unsigned division/remainder,
logical/arithmetic operations, shifts, signed/unsigned comparisons, repeated SSA
uses, conditional branches, loop counts 0/1/7/100, globals and local storage,
void calls with memory effects, four-argument calls with caller-live values,
recursive Fibonacci through input 8, and cyclic parallel PHI copies through
50 loop iterations.

## RTL and memory configurations

* Baseline: single-cycle Squall, Icarus Verilog.
* Pipeline: full six-stage pipeline with stall-based hazards, Icarus Verilog.
* Production: the actual `create_squall_prod.py` generator, including native
  instruction cache, write-back HPDcache, sliding-window stack cache, shared
  external 1R/1W memory interface, and full pipeline; simulated with Verilator.
  Memory response latency settings were 0, 5, and 11. Delayed-memory responses
  use the upstream harness's poison data until a response is ready.

The core cycle counts are recorded before cache flush and memory quiescence.
They reflect these simulation memory settings and are not silicon performance
comparisons between different clock paths.

Pinned Isacomp: `899ee4456cabd88deb7d2ab1088ad721178dfb8b`.
Pinned HPDcache: `39fabbee1fa90bd4355444c042ed7568573cbfc1`.
Pinned Zig interpreter: `03dcc01ffd68abe5cf6169bd2af6090e0f398077` with Zig 0.16.0.
Simulator versions and generated RTL/testbench SHA-256 hashes are recorded per
configuration in `results.json` and simulator cache provenance files.

## Passing criteria

Every run required successful drain, status 1, the independently expected answer,
a successful final simulator marker, no simulator errors, and token-stack bounds.
Production runs additionally had to finish all cache flushes and write quiescence
before the memory dump, with explicit timeout failures in the testbench.

The baseline matched useful-token order exactly. For pipelined variants, initial
argument delivery may be scheduled differently. All 225 traces were checked with
a stricter serial-ABI audit: the bootstrap token multisets must agree, and the
entire useful-token sequence from root function entry onward must match exactly.
Only the bootstrap order differed in 23 cases per pipelined configuration.

Final memory was compared across code, constants, globals, all frame slots, and
other storage. The only excluded state was the reserved token stack scratch and
the undefined payloads of spent bootstrap Once slots. There were **zero final
memory mismatches**.

Additional runner tests deliberately corrupted a launch argument and an unused
frame word. Both were detected; the latter was rejected even though result,
status, and token trace still matched. The package regression passed **295 tests**,
and the final focused RTL/comparison checks passed **10 tests**, including four
new tests that reject reordering after entry or missing argument delivery.

## Example production results, latency setting 5

| Program | Inputs | Result | Useful tokens | Core cycles |
| --- | --- | ---: | ---: | ---: |
| Recursive Fibonacci | 8 | 21 | 5,132 | 371,767 |
| Loop with global memory and void calls | 100 | 403 | 10,259 | 815,378 |
| Two four-argument calls with caller-live values | 3, 4, 10, 4 | 47 | 307 | 22,347 |

## Reproduce

Use the compiler environment with llvmlite, Zig 0.16.0, Icarus Verilog, and Verilator.
From the compiler source repository:

```sh
python llvm/utils/squall/test_rtl.py \
  --clang ../squall-llvm-build/bin/clang \
  --isacomp-dir /absolute/path/to/isacomp-checkout \
  --reference-dir /absolute/path/to/squall-v2-reference-impl \
  --output ../squall-rtl-compiler-regression
```

Initialize only the required HPDcache submodule if needed with
`git submodule update --init vendor/cv-hpdcache` in Isacomp. Both checkouts must
match the pinned revisions; the runner rejects modified tracked dependencies.

For one existing image:

```sh
squall-rtl ../squall-rtl-compiler-regression/images/calls-0 \
  --isacomp-dir /absolute/path/to/isacomp-checkout \
  --reference-dir /absolute/path/to/squall-v2-reference-impl \
  --variant production --memory-latency 11
```

## Saved artifacts and scope

The local output directory is
`../squall-rtl-compiler-regression`.
It contains `results.json`, the LLVM IR, the independent host C library, each
image and manifest, complete reference/RTL traces, and per-run result files.
The adjacent `squall-rtl-compiler-regression.log` records the 225 outcomes.

These are directed compiler integration tests of fresh, immutable-code images.
They do not establish general RTL correctness or tapeout signoff. Randomized
memory timing and backpressure, full concurrency stress, interrupts/reset during
execution, code-reload coherence, formal properties, and physical checks remain
separate verification work. Native `llc`/MC/ELF Squall emission is still unsupported;
the tested path is native Clang LLVM IR followed by `squall-compile` image emission.
