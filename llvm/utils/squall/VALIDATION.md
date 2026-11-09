# Validation

## Current contract (2026-11-09)

Image format `squall-v2-image-2`, ABI `squall-frame-slots-2`, Isacomp
`squall-v2-rebase` at `a21ccd67752c7b8bd9523818aa77b972f13ddb16` (CVA6
`81245a47`), reference interpreter `5882fe54` with
`squall_tools/reference-rebase-rtl.patch`. Measured on macOS (arm64) with
Icarus Verilog 13.0 and Verilator 5.052, on the working tree after the
November 9 silicon-readiness fixes.

| Check | Command | Result |
| --- | --- | --- |
| Squall lit tests | `llvm-lit llvm/test/{CodeGen,MC}/Squall clang/test/{CodeGen,Preprocessor}/squall.c` | 16/16 passed |
| Execution tests | `pytest llvm/utils/squall/tests` (Embench excluded) with Clang, the reference, the RTL, and `sqasm.py` | 859 passed, 0 failed, 0 skipped |
| RTL regression | `rtl_regression.py --opt 0 1 2 --random-seeds 0 1 7 42 113 2026 --jitter` | 345 cases, 2415 RTL runs, 0 failures |
| Embench-IoT 1.0 on the interpreter | `embench.py --opt 0 2` | 38/38 passed |
| Embench-IoT 1.0, `-O2`, on the production core | each image through `squall_tools.rtl.run_rtl` | 18 of 19 benchmarks at memory latency 5, and the 13 of up to 2M tokens again at latency 11 with jitter: 31 runs, 0 failures. `cubic` needs a 983040-byte image and cannot run on the 512 KiB core |

Every RTL run matched the interpreter's result, status, executed tokens, and
final observable memory. The Embench runs execute up to 17.6M tokens (215M
core cycles for `nbody`).

Not covered: the FPGA generator (`create_squall_fpga.py`), any post-synthesis
or gate-level netlist, Embench at `-O0`/`-O1` on the RTL, and `cubic` on any
RTL. [BRINGUP.md](BRINGUP.md) describes what a loader and a board must do.

## Earlier contract (2026-10-24)

> **Superseded.** The results below were measured for image format
> `squall-v2-image-1` / ABI `squall-frame-slots-1`, on Isacomp
> `squall-v2-hpdcache` (`899ee445`).

Results for the backend as committed, measured on macOS (arm64) on 2026-10-24
with the default build (`build.py --check`, Squall as the only target, Clang
enabled, assertions on).

| Check | Command | Result |
| --- | --- | --- |
| Build and checks | `build.py --check` | Passed |
| Target parser unit tests | `TargetParserTests` | 670 passed, 3 skipped (AIX host detection) |
| Squall lit tests | `llvm-lit llvm/test/{CodeGen,MC}/Squall clang/test/{CodeGen,Preprocessor}/squall.c` | 10/10 passed |
| Upstream lit suites | `llvm-lit llvm/test/{Assembler,Verifier,FileCheck,TableGen}` | 1444 discovered, 0 failures |
| Execution tests | `pytest llvm/utils/squall/tests` with Clang, the Zig reference, and RTL | 357 passed, 0 skipped |
| RTL regression | `rtl_regression.py --opt 0 1 2 --random-seeds 0 1 7 42 113 2026 --stall-seeds 17 91` | 252 cases, 1764 RTL runs, 0 failures |

Pinned dependencies: the Zig reference interpreter at
`03dcc01ffd68abe5cf6169bd2af6090e0f398077` (Zig 0.16.0), the Isacomp RTL at
`899ee4456cabd88deb7d2ab1088ad721178dfb8b`, Icarus Verilog 13.0, and Verilator
5.052.

## What the oracles check

* **Host execution.** C tests compile the same source for the host, call it
  through `ctypes`, and require the Squall image to return the same 32-bit
  result. IR tests compare against values computed in Python.
* **Reference interpreter.** Every image must reach quiescence on the pinned
  Zig interpreter with status 1 and the expected result, or with the expected
  error status for the call-depth and bad-address tests.
* **RTL.** The baseline and pipelined cores (Icarus) and the production core
  with HPDcache (Verilator) must match the interpreter's result, status, final
  observable memory, and token order: exact on the baseline, and from entry
  onward on the overlapped pipelines. The production core runs with fixed memory
  latencies of 0, 5, and 11 cycles and with two seeded random-stall schedules.

## RTL regression matrix

| Configuration | Passed |
| --- | ---: |
| Baseline (Icarus) | 252/252 |
| Pipelined (Icarus) | 252/252 |
| Production, latency 0 (Verilator) | 252/252 |
| Production, latency 5 | 252/252 |
| Production, latency 11 | 252/252 |
| Production, latency 5, stall seed 17 (jitter 19) | 252/252 |
| Production, latency 5, stall seed 91 (jitter 19) | 252/252 |

The matrix covers `rtl_kernels.c`, `memory_kernels.c`, `programs.c`, and six
seeded random programs, each at `-O0`, `-O1`, and `-O2`, on seven RTL
configurations.

## Not covered

* CI runs the LLVM-only build, so it skips the C tests (`test_c.py` and the
  Clang diagnostic test) and runs RTL only when `SQUALL_RTL_ENABLED` is set and
  `ISACOMP_READ_TOKEN` grants access to the private RTL repository.
* Only macOS arm64 hosts have been measured; the CI job targets Ubuntu.
* There are no performance comparisons with other compilers: the backend
  executes serially and has not been tuned.
