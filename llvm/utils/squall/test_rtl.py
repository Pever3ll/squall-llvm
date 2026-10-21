#!/usr/bin/env python3
"""Compile C with the native Squall Clang and test emitted images on real RTL."""
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

from squall_compiler.compiler import compile_ir
from squall_compiler.random_programs import generate
from squall_compiler.reference import run_image
from squall_compiler.rtl import run_rtl, build_simulator, VARIANTS

ROOT = Path(__file__).resolve().parents[3]
CASES = {
    "add": [(7, 35), (-1, 1), (-100, 25)],
    "fanout": [(0,), (1,), (-1,), (-2147483648,), (2147483647,)],
    "unsigned_divrem": [(0, 3), (-1, 3), (-2147483648, 7), (-1, -1)],
    "signed_divrem": [(-7, 3), (7, -3), (-7, -3), (-2147483648, 31)],
    "shifts": [(-1, 1), (-2147483648, 31), (2147483647, 0)],
    "comparisons": [(0, 0), (-1, 0), (0, -1), (-2147483648, 2147483647), (2147483647, -2147483648)],
    "branch": [(7, -29, 1), (7, -29, 0)],
    "loop": [(0,), (1,), (7,), (100,)],
    "memory_calls": [(0,), (1,), (10,), (100,)],
    "fibonacci": [(0,), (1,), (5,), (8,)],
    "calls": [(3, 4, 10, 4), (7, -3, -5, 2)],
}
PHI_SWAP = """define i32 @main(i32 %n) {
entry: br label %loop
loop:
  %a = phi i32 [11, %entry], [%b, %body]
  %b = phi i32 [29, %entry], [%a, %body]
  %i = phi i32 [0, %entry], [%next, %body]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body: %next = add i32 %i, 1
  br label %loop
exit: %r = sub i32 %a, %b
  ret i32 %r
}"""


def signed32(value):
    value &= 0xffffffff
    return value - (1 << 32) if value >= 1 << 31 else value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True, help="newly built Squall Clang")
    parser.add_argument("--frontend-target", default="squallv2-unknown-none")
    parser.add_argument("--include-memory", action="store_true")
    parser.add_argument("--random-seeds", nargs="*", type=int, default=[])
    parser.add_argument("--stall-seeds", nargs="*", type=int, default=[])
    parser.add_argument("--stall-jitter", type=int, default=19)
    parser.add_argument("--host-clang", default=shutil.which("clang"), help="independent host C compiler")
    parser.add_argument("--isacomp-dir", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    parser.add_argument("--latencies", nargs="+", type=int, default=[0, 5, 11])
    args = parser.parse_args()
    if args.reference_dir:
        os.environ["SQUALL_REFERENCE_DIR"] = str(args.reference_dir.resolve())
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "llvm/tools/squall-compiler/examples/rtl_kernels.c"
    ir_path = args.output / "kernels.ll"
    commands = [[str(args.clang.resolve()), f"--target={args.frontend_target}", "-O0", "-S", "-emit-llvm",
                 str(source), "-o", str(ir_path)],
                [args.host_clang, "-O0", "-shared", "-fPIC", str(source), "-o", str(args.output / "host-kernels.so")]]
    for command in commands:
        subprocess.run(command, check=True)
    host = ctypes.CDLL(str(args.output / "host-kernels.so"))
    cases = []
    for entry, arguments in CASES.items():
        function = getattr(host, entry)
        function.restype = ctypes.c_uint32
        for index, inputs in enumerate(arguments):
            function.argtypes = [ctypes.c_int32] * len(inputs)
            cases.append((f"{entry}-{index}", ir_path.read_text(), entry, inputs, signed32(function(*inputs))))
    for count in [0, 1, 2, 7, 50]:
        cases.append((f"parallel-phi-{count}", PHI_SWAP, "main", [count], 18 if count & 1 else -18))
    def c_cases(source, stem, entries):
        ir = args.output / f"{stem}.ll"
        library = args.output / f"{stem}.so"
        commands = [[str(args.clang.resolve()), f"--target={args.frontend_target}", "-O0", "-S", "-emit-llvm", str(source), "-o", str(ir)],
                    [args.host_clang, "-O0", "-shared", "-fPIC", str(source), "-o", str(library)]]
        for command in commands:
            subprocess.run(command, check=True)
        native = ctypes.CDLL(str(library))
        for entry, inputs in entries:
            function = getattr(native, entry)
            function.argtypes, function.restype = [ctypes.c_int32] * len(inputs), ctypes.c_uint32
            name = f"{stem}-{entry}-" + "-".join(map(str, inputs))
            cases.append((name, ir.read_text(), entry, inputs, signed32(function(*inputs))))
    if args.include_memory:
        source = ROOT / "llvm/tools/squall-compiler/examples/memory_kernels.c"
        entries = ([("dot", [n]) for n in (0, 1, 4, 8)]
                   + [("reduction", [n]) for n in (0, 8)]
                   + [("local_array", [n]) for n in (0, 3)]
                   + [("matrix_product", [])]
                   + [("initialized_local", [n]) for n in range(4)])
        c_cases(source, "memory", entries)
    for seed in args.random_seeds:
        source = args.output / f"seed-{seed}.c"
        source.write_text(generate(seed))
        c_cases(source, f"seed-{seed}", [("kernel", [v, n]) for v, n in ((0, 0), (-1, 8), (seed & 0x7fffffff, 5))])
    configurations = [(variant, latency, 0, 0) for variant in args.variants
                      for latency in (args.latencies if variant == "production" else [0])]
    if args.stall_seeds:
        if "production" not in args.variants:
            parser.error("--stall-seeds requires --variants production")
        configurations.extend(("production", 5, seed, args.stall_jitter) for seed in args.stall_seeds)
    def config_key(variant, latency, seed, jitter):
        return f"{variant}-lat{latency}" + (f"-seed{seed}-jitter{jitter}" if jitter else "")
    report = {"commands": commands, "cases": len(cases), "configurations": {}, "runs": []}
    for variant, latency, seed, jitter in configurations:
        key = config_key(variant, latency, seed, jitter)
        print(f"Building {key}", flush=True)
        report["configurations"][key] = build_simulator(args.isacomp_dir, variant=variant, memory_latency=latency)
    for name, ir, entry, arguments, expected in cases:
        image = args.output / "images" / name
        # A generous physical stack region accommodates each implementation's
        # halt padding; runtime assertions still enforce its declared bound.
        compiled = compile_ir(ir, entry=entry, arguments=arguments, stack_tokens=1024)
        compiled.write(image)
        (image / "input.ll").write_text(ir)
        reference = run_image(image, trace=True)
        if not reference["quiescent"] or reference["status"] != 1 or reference["result"] != expected:
            raise RuntimeError(f"{name}: compiler/reference disagrees with independent host execution: {reference}, expected {expected}")
        for variant, latency, seed, jitter in configurations:
            row = {"case": name, "entry": entry, "arguments": arguments, "expected": expected,
                   "configuration": config_key(variant, latency, seed, jitter),
                   "code_words": len(compiled.assembly.records), "initialized_words": len(compiled.words),
                   "reference_cycles": reference["cycles"], "peak_tokens": reference["peak_tokens"]}
            try:
                result = run_rtl(image, isacomp=args.isacomp_dir, variant=variant, memory_latency=latency, stall_seed=seed, stall_jitter=jitter)
                row.update({k: v for k, v in result.items() if k != "simulator"})
                row["passed"] = result["result"] == expected
            except (OSError, RuntimeError, ValueError) as exc:
                row.update(passed=False, error=str(exc))
            report["runs"].append(row)
            (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(name, " ".join(f"{r['configuration']}={'PASS' if r['passed'] else 'FAIL'}"
                             for r in report["runs"][-len(configurations):]), flush=True)
    passed = Counter(row["configuration"] for row in report["runs"] if row["passed"])
    report["passed"] = dict(passed)
    report["failures"] = sum(not row["passed"] for row in report["runs"])
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# Compiler-to-RTL regression", "", f"{len(cases)} independent host/reference cases; {len(report['runs'])} RTL runs; {report['failures']} failures.", "",
             "The LLVM frontend target was `" + args.frontend_target + "`. Expected C answers came from a separately compiled native host library. Code size is measured in 64-bit records; cycles are simulation counts, not measured silicon performance.", "",
             "| Configuration | Passed | Useful tokens | Core cycles |", "| --- | ---: | ---: | ---: |"]
    for key in report["configurations"]:
        rows = [row for row in report["runs"] if row["configuration"] == key]
        lines.append(f"| {key} | {sum(row['passed'] for row in rows)}/{len(rows)} | {sum(row.get('tokens', 0) for row in rows)} | {sum(row.get('wall_cycles', 0) or 0 for row in rows)} |")
    lines.extend(["", "Every passing run matched result, status, exact post-entry token order, and final observable memory. Seeded configurations vary read/write response delays independently while retaining poisoned unready read data and held responses. Sources, IR, memory images, simulator provenance, and traces are retained in this directory.", ""])
    (args.output / "REPORT.md").write_text("\n".join(lines))
    print(json.dumps({"cases": len(cases), "passed": report["passed"], "failures": report["failures"]}, indent=2))
    return int(report["failures"] != 0)


if __name__ == "__main__":
    sys.exit(main())
