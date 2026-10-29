#!/usr/bin/env python3
"""Compile C with the built Squall Clang and run every image on the RTL matrix.

Each case is checked three ways: the C source compiled for the host and run
natively gives the expected answer; the pinned Zig interpreter must agree; and
each RTL configuration must match the interpreter's result, token order, and
final memory.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from squall_tools.image import load_image
from squall_tools.random_programs import generate
from squall_tools.reference import run_image
from squall_tools.rtl import VARIANTS, build_simulator, run_rtl

EXAMPLES = Path(__file__).resolve().parent / "examples"
CASES = {
    "rtl_kernels.c": {
        "add": [(7, 35), (-1, 1), (-100, 25)],
        "fanout": [(0,), (1,), (-1,), (-2147483648,), (2147483647,)],
        "unsigned_divrem": [(0, 3), (-1, 3), (-2147483648, 7), (-1, -1)],
        "signed_divrem": [(-7, 3), (7, -3), (-7, -3), (-2147483648, 31)],
        "shifts": [(-1, 1), (-2147483648, 31), (2147483647, 0)],
        "comparisons": [(0, 0), (-1, 0), (0, -1), (-2147483648, 2147483647)],
        "branch": [(7, -29, 1), (7, -29, 0)],
        "loop": [(0,), (1,), (7,), (100,)],
        "memory_calls": [(0,), (1,), (10,), (100,)],
        "fibonacci": [(0,), (1,), (5,), (8,)],
        "calls": [(3, 4, 10, 4), (7, -3, -5, 2)],
    },
    "memory_kernels.c": {
        "dot": [(0,), (1,), (4,), (8,)],
        "reduction": [(0,), (8,)],
        "local_array": [(0,), (3,)],
        "matrix_product": [()],
        "initialized_local": [(0,), (1,), (2,), (3,)],
    },
    "bytes.c": {
        "string_length": [(0,), (9,)],
        "reverse": [(5,)],
        "concatenate": [(0,), (3,)],
        "sort_letters": [(6,)],
        "signed_bytes": [(-77,)],
        "shorts": [(-3,)],
        "records_by_value": [(4,)],
        "packed_fields": [(12345,)],
        "adjacent_bytes": [(-91,)],
        "variable_copies": [(13,), (29,)],
        "wide_division": [(-99999, 13), (-2147483648, 0)],
    },
    "callbacks.c": {
        "dispatch": [(1, 7, 5), (3, 2, 9)],
        "folds": [(3,)],
        "handler_table": [(4,)],
        "sort_with": [(1,), (2,)],
        "recursive_pointer": [(17,)],
        "returned_pointer": [(0,), (5,)],
    },
    "parallel.c": {
        "chains": [(1,), (37,)],
        "horner": [(5,), (-12,)],
        "sum_and_hash": [(3,)],
        "call_beside_arithmetic": [(27,)],
        "two_frame_arrays": [(9,)],
    },
    "programs.c": {
        "points": [(0,), (3,), (-7,)],
        "classify": [(0,), (2,), (7,), (-3,), (100,)],
        "sort": [(1,), (12345,)],
        "gcd": [(1071, 462), (-48, 18)],
        "collatz": [(27,)],
        "histogram": [(40,)],
    },
}


def signed32(value):
    value &= 0xffffffff
    return value - (1 << 32) if value >= 1 << 31 else value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path, required=True, help="built LLVM bin directory with clang")
    parser.add_argument("--isacomp-dir", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--opt", nargs="+", default=["0", "2"], choices=["0", "1", "2", "3", "s", "z"],
                        help="optimization levels, e.g. --opt 0 1 2")
    parser.add_argument("--random-seeds", nargs="*", type=int, default=[])
    parser.add_argument("--stall-seeds", nargs="*", type=int, default=[])
    parser.add_argument("--stall-jitter", type=int, default=19)
    parser.add_argument("--host-clang", default=shutil.which("clang"), help="independent host C compiler")
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    parser.add_argument("--latencies", nargs="+", type=int, default=[0, 5, 11])
    parser.add_argument("--fork-join", action="store_true",
                        help="compile with -squall-fork-join (parallel lanes)")
    args = parser.parse_args()
    if args.reference_dir:
        os.environ["SQUALL_REFERENCE_DIR"] = str(args.reference_dir.resolve())
    if args.stall_seeds and "production" not in args.variants:
        parser.error("--stall-seeds requires --variants production")
    args.opt = [f"-O{level}" for level in args.opt]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tools = {name: str(args.llvm_bin.resolve() / name) for name in ("clang", "llvm-objcopy", "llvm-nm")}

    sources = {name: EXAMPLES / name for name in CASES}
    cases = {name: dict(entries) for name, entries in CASES.items()}
    for seed in args.random_seeds:
        path = output / f"seed-{seed}.c"
        path.write_text(generate(seed))
        sources[path.name] = path
        cases[path.name] = {"kernel": [(0, 0), (-1, 8), (seed & 0x7fffffff, 5)]}

    configurations = [(variant, latency, 0, 0) for variant in args.variants
                      for latency in (args.latencies if variant == "production" else [0])]
    configurations += [("production", 5, seed, args.stall_jitter) for seed in args.stall_seeds]

    def key(variant, latency, seed, jitter):
        return f"{variant}-lat{latency}" + (f"-seed{seed}-jitter{jitter}" if jitter else "")

    report = {"compiler": "llvm squall backend", "optimization": args.opt, "fork_join": args.fork_join,
              "configurations": {}, "runs": []}
    for configuration in configurations:
        print(f"Building {key(*configuration)}", flush=True)
        report["configurations"][key(*configuration)] = build_simulator(
            args.isacomp_dir, variant=configuration[0], memory_latency=configuration[1])

    case_count = 0
    for source_name, entries in cases.items():
        source = sources[source_name]
        library = output / f"{source.stem}-host.so"
        subprocess.run([args.host_clang, "-O0", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
        for level in args.opt:
            for entry, inputs in entries.items():
                obj = output / "objects" / f"{source.stem}{level}-{entry}.o"
                obj.parent.mkdir(parents=True, exist_ok=True)
                fork = ["-mllvm", "-squall-fork-join"] if args.fork_join else []
                subprocess.run([tools["clang"], "--target=squallv2", level, "-c", "-mllvm",
                                f"-squall-entry={entry}", *fork, str(source), "-o", str(obj)], check=True)
                for index, arguments in enumerate(inputs):
                    case_count += 1
                    # A private copy per run: some examples modify globals.
                    copy = output / "host" / f"{source.stem}{level}-{entry}-{index}.so"
                    copy.parent.mkdir(exist_ok=True)
                    shutil.copy(library, copy)
                    function = getattr(ctypes.CDLL(str(copy)), entry)
                    function.restype = ctypes.c_uint32
                    function.argtypes = [ctypes.c_int32] * len(arguments)
                    expected = signed32(function(*arguments))
                    name = f"{source.stem}{level}-{entry}-" + "_".join(map(str, arguments))
                    image = output / "images" / name
                    load_image(obj, image, arguments, objcopy=tools["llvm-objcopy"], nm=tools["llvm-nm"])
                    reference = run_image(image, trace=True)
                    if not reference["quiescent"] or reference["status"] != 1 or reference["result"] != expected:
                        raise RuntimeError(f"{name}: reference {reference} disagrees with host {expected}")
                    for configuration in configurations:
                        variant, latency, seed, jitter = configuration
                        row = {"case": name, "opt": level, "entry": entry, "arguments": list(arguments),
                               "expected": expected, "configuration": key(*configuration),
                               "reference_cycles": reference["cycles"], "peak_tokens": reference["peak_tokens"]}
                        try:
                            result = run_rtl(image, isacomp=args.isacomp_dir, variant=variant,
                                             memory_latency=latency, stall_seed=seed, stall_jitter=jitter)
                            row.update({k: v for k, v in result.items() if k != "simulator"})
                            row["passed"] = result["result"] == expected
                        except (OSError, RuntimeError, ValueError) as exc:
                            row.update(passed=False, error=str(exc))
                        report["runs"].append(row)
                    print(name, " ".join(f"{r['configuration']}={'PASS' if r['passed'] else 'FAIL'}"
                                         for r in report["runs"][-len(configurations):]), flush=True)
                    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")

    report["cases"] = case_count
    report["passed"] = dict(Counter(row["configuration"] for row in report["runs"] if row["passed"]))
    report["failures"] = sum(not row["passed"] for row in report["runs"])
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# Squall backend RTL regression", "",
             f"{case_count} cases; {len(report['runs'])} RTL runs; {report['failures']} failures.", "",
             "Images were compiled by the built Squall Clang at " + ", ".join(args.opt) +
             (" with -squall-fork-join" if args.fork_join else "") +
             ". Expected answers came from the same C compiled for the host and run natively. "
             "Cycle counts are simulation counts, not silicon measurements.", "",
             "| Configuration | Passed | Useful tokens | Core cycles |", "| --- | ---: | ---: | ---: |"]
    for name in report["configurations"]:
        rows = [row for row in report["runs"] if row["configuration"] == name]
        lines.append(f"| {name} | {sum(row['passed'] for row in rows)}/{len(rows)} | "
                     f"{sum(row.get('tokens', 0) for row in rows)} | "
                     f"{sum(row.get('wall_cycles', 0) or 0 for row in rows)} |")
    lines += ["", "A passing run matched the result, status, token order (exact on the baseline, "
              "post-entry on overlapped pipelines), and final observable memory.", ""]
    (output / "REPORT.md").write_text("\n".join(lines))
    print(json.dumps({"cases": case_count, "passed": report["passed"], "failures": report["failures"]}, indent=2))
    return int(report["failures"] != 0)


if __name__ == "__main__":
    sys.exit(main())
