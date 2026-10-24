# Squall syntax and memory validation — 2026-10-20

Historical report for the standalone compiler/scaffolding revision. Native LLVM
integration added later is described in [NATIVE-VALIDATION.md](NATIVE-VALIDATION.md).

**532/532 RTL runs passed**, covering 76 cases on seven configurations. All images
were generated from LLVM IR emitted by the built Squall Clang frontend. C results
came from independently executing the same source compiled with host Clang.
Every case also ran on the actual pinned Zig interpreter.

| Configuration | Passed | Useful tokens, summed | Core cycles, summed |
| --- | ---: | ---: | ---: |
| baseline-lat0 | 76/76 | 109629 | 114320 |
| pipeline-lat0 | 76/76 | 109629 | 646034 |
| production-lat0 | 76/76 | 109629 | 6120931 |
| production-lat5 | 76/76 | 109629 | 9109506 |
| production-lat11 | 76/76 | 109629 | 12050796 |
| production-lat5-seed17-jitter19 | 76/76 | 109629 | 13810461 |
| production-lat5-seed91-jitter19 | 76/76 | 109629 | 13800582 |

The cases comprise the earlier 45 scalar/call/PHI inputs, 13 memory benchmark
inputs, and 18 inputs from six seeded C programs. Memory cases exercise indexed
global/local arrays, pointer arguments across calls, pointer spills, nested GEP,
initialization through memcpy, reduction, dot product, and a small matrix product.
Seeded programs combine initialized arrays, pointer select/branches, unsigned
arithmetic, nonzero division, valid shifts, and bounded loops.

Every run checked result, status, completion/drain, token-stack bounds, exact
useful-token order after entry, and final memory, including data arenas, runtime
mailboxes, code, constants, and frame storage. Reserved stack scratch and undefined
spent Once payloads are the only memory exclusions. Production runs completed
cache flush and external-write quiescence before comparison. Seeded delays vary
read/write response timing while retaining poison until ready and held responses.
No processor RTL or Isacomp optimization sources were modified.

The package suite passed **349 tests**, including production RTL delay
reproducibility; three additional memory-boundary cases passed in a subsequent
focused run. A separate **43-test** run exercised the upstream i386 frontend
path used by CI.

The final-source audit regenerated all 76 images and confirmed byte-identical
machine images and reassembled listings. Only additive allocation diagnostics
were refreshed in manifests. This ties the simulation evidence to the final
compiler rather than an intermediate implementation.

Pinned Isacomp: `899ee4456cabd88deb7d2ab1088ad721178dfb8b`.
Pinned HPDcache: `39fabbee1fa90bd4355444c042ed7568573cbfc1`.
Pinned reference: `03dcc01ffd68abe5cf6169bd2af6090e0f398077`, Zig 0.16.0.
The ABI is `squall-cell-memory-2`: a four-byte logical i32/pointer cell occupies
one eight-byte Squall matching slot. See the compiler README for mapping,
alignment, mailbox serialization, and supported-memory limits.

Reproduce from the compiler repository with its installed Python package:

```sh
python llvm/utils/squall/test_rtl.py \
  --clang ../squall-llvm-build/bin/clang \
  --isacomp-dir ../isacomp-squall-2026-10-20 \
  --reference-dir /absolute/path/to/squall-v2-reference-impl \
  --output ../squall-memory-regression \
  --include-memory --random-seeds 0 1 7 42 113 2026 --stall-seeds 17 91
```

Local artifacts are under
`../squall-memory-regression`:
`results.json`, `REPORT.md`, `FINAL-SOURCE-AUDIT.md`, generated C/LLVM IR, host
libraries, manifests, assembly, complete images, traces, and per-run results.
Recheck the saved images with `python llvm/utils/squall/audit_images.py
../squall-memory-regression`. Per-image `input.ll` and
`final-source-audit.json` retain exact IR and compiler source hashes.

GitHub CI jobs have been added for reference and RTL differential execution.
The private RTL job requires the access setup documented in the compiler README.
The CI frontend uses upstream Clang's i386 target for p32 IR; GitHub execution
has not been observed yet.

These checks cover this serial compiler ABI and fresh immutable-code launches.
Native llc/MC/ELF emission, byte-addressed C interoperability, concurrent scheduling,
interrupt/reset stress, full protocol verification, formal proofs, and physical
signoff remain outstanding. Shared runtime mailboxes must be redesigned before
memory requests can overlap. The memory guards check alignment/reserved-low-memory/
physical bounds; they are not object-provenance or array-bound sanitizers.
