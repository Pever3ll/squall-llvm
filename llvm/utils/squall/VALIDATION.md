# Native Squall LLVM/Clang validation — 2026-10-20

The complete configured LLVM **23.1.2** / Clang tools build succeeded on arm64
macOS with Apple Clang 16.0.0. All default build targets completed, followed by a
successful incremental rebuild; Ninja now reports **no work to do**.

Build directory: `../squall-llvm-build`.
Source: `../squall-llvm-review`,
branch `codex/squall-ir-compiler`.

## Repairs

* Restored the omitted libc sources from matching upstream LLVM 23.1.2 commit
  `85ac560262434c9ccfc0c183ec22d4138ed647fb`, rather than mixing source versions.
* Restored upstream LLVM lit infrastructure and selected regression suites that
  were missing from the original import. Source provenance is recorded in
  `libc/UPSTREAM-SOURCE.json` and `llvm/test/UPSTREAM-SOURCE.json`.
* Supplied minimal MC context metadata in the Squall target machine so requesting
  unsupported native emission produces a diagnostic instead of a null dereference.
* Added repeatable source restoration, a native CMake cache, native Squall frontend
  and diagnostic tests, and build instructions under `llvm/utils/squall/`.

## Results

| Check | Result |
| --- | --- |
| Fresh configuration through build helper | Passed |
| Complete default build, including LLVM tools and Clang | Passed |
| Incremental rebuild | No work to do |
| Target parser unit tests | 670 passed; 3 AIX-only tests skipped; 0 failures |
| Native Squall lit tests | All 3 passed |
| Assembler / Verifier / FileCheck / TableGen | 1,444 discovered; successful run, no failures |
| Complete Clang Preprocessor directory | 375 passed; 14 unsupported/skipped; 0 failures |
| Native Clang → LLVM IR → llvm-as → opt verification | Passed |
| Native Clang → LLVM IR → Squall → Zig interpreter | Returned 45; successful quiescent completion |
| Same C-derived Squall image → Isacomp baseline RTL | Returned 45; all 803 useful token entries match reference |

The native C-derived example used two live token slots, reached a peak of two,
and completed in 833 reference interpreter cycles. Its image and traces are in
`../squall-compiler-tests/native-clang/` relative to the build directory.

The complete first build log is `../squall-native-build.log`; the incremental
build and native checks are in `../squall-native-check.log`. Clang Preprocessor
results are in `../squall-native-preprocessor.xml` and its adjacent `.log`.
Machine-readable validation is `squall-validation.json` in the build directory.

## Rebuild

From the source repository, with Ninja on PATH:

```sh
python3 llvm/utils/squall/build.py --build-dir ../squall-llvm-build --jobs 6 --check
```

The configured build uses MinSizeRel, shared LLVM/Clang libraries, assertions,
no debug symbols, no precompiled headers, and one concurrent link. Only the
Squall backend and the Clang project are selected. Host or other architecture
backends, extra projects, and runtimes are not included in this configuration.

## Scope

This validates building the native LLVM tools and Clang, target discovery,
LLVM IR processing, and the executable standalone compiler path. Native Squall
MachineInstr selection and MC/ELF emission remain unimplemented. The native
`llc` rejection is explicitly tested. The entire upstream LLVM regression corpus
was not restored or run; the selected suites and existing Clang tests/unit tests
are identified above. Production cached/pipelined hardware is not validated by
this build report.
