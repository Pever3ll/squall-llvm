#!/usr/bin/env python3
"""Build Embench-IoT benchmarks with the Squall toolchain and run them.

Each benchmark is copied to the output directory with LOCAL_SCALE_FACTOR set
to 1, since a simulated run of the full scale takes hours; Embench's
self-checks verify the work of each repetition, so the verdict does not
depend on the count. A small entry function runs initialise/benchmark/verify
and returns verify's result, 1 for correct. The same files built for the
host give the expected verdict. Images run on the pinned Zig interpreter
and, with --isacomp-dir, on the RTL cores (images up to 512 KiB).

    python3 embench.py --embench embench-iot --llvm-bin ../squall-llvm-build/bin --output out
    python3 embench.py ... --opt 0 2 -- crc32 cubic     # selected benchmarks

A program too large for the default image is rebuilt once at the size the
compiler suggests.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from squall_tools.image import load_image
from squall_tools.reference import run_image

ENTRY = """#include "support.h"
void initialise_board(void) {}
void start_trigger(void) {}
void stop_trigger(void) {}
int embench_main(void) {
  initialise_board();
  initialise_benchmark();
  warm_caches(0);
  start_trigger();
  int result = benchmark();
  stop_trigger();
  return verify_benchmark(result);
}
#ifdef HOST
#include <stdio.h>
int main(void) { printf("%d\\n", embench_main()); return 0; }
#endif
"""
TOO_LARGE = re.compile(r"use -squall-memory-bytes=(\d+) or more")


def prepare(embench, name, output):
    work = output / name
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for source in (embench / "src" / name).iterdir():
        text = source.read_text(errors="replace")
        (work / source.name).write_text(
            re.sub(r"#define\s+LOCAL_SCALE_FACTOR\s+\d+", "#define LOCAL_SCALE_FACTOR 1", text))
    for support in ("beebsc.c", "beebsc.h", "support.h"):
        shutil.copy(embench / "support" / support, work / support)
    (work / "boardsupport.h").write_text("#define CPU_MHZ 1\n")
    (work / "entry.c").write_text(ENTRY)
    return work, sorted(str(p) for p in work.glob("*.c"))


def build(binary, level, defines, sources, image):
    """Compile, retrying once with the image size the compiler asks for."""
    options = []
    for _ in range(2):
        run = subprocess.run([str(binary / "clang"), "--target=squallv2", level, *defines, *options,
                              "-mllvm", "-squall-entry=embench_main", *sources, "-o", str(image)],
                             capture_output=True, text=True)
        match = TOO_LARGE.search(run.stderr)
        if run.returncode == 0 or not match:
            break
        options = ["-mllvm", f"-squall-memory-bytes={match.group(1)}"]
    return run, options


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--embench", type=Path, required=True, help="Embench-IoT checkout (1.0)")
    parser.add_argument("--llvm-bin", type=Path, required=True, help="built LLVM bin directory with clang")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--opt", nargs="+", default=["2"], choices=["0", "1", "2", "s", "z"])
    parser.add_argument("--host-clang", default=shutil.which("clang"), help="independent host C compiler")
    parser.add_argument("--isacomp-dir", type=Path, help="also run on the RTL cores of this checkout")
    parser.add_argument("--rtl-timeout", type=int, default=3600,
                        help="seconds per RTL simulation (benchmarks run for millions of cycles)")
    parser.add_argument("benchmarks", nargs="*", help="benchmark names (default: all)")
    args = parser.parse_args()
    names = args.benchmarks or sorted(p.name for p in (args.embench / "src").iterdir() if p.is_dir())
    rows = []
    for level in (f"-O{o}" for o in args.opt):
        for name in names:
            work, sources = prepare(args.embench, name, args.output / level / name)
            defines = ["-DHAVE_BOARDSUPPORT_H", f"-I{work}"]
            row = {"benchmark": name, "level": level}
            host = subprocess.run([args.host_clang, "-O2", "-DHOST", "-ffp-contract=off", *defines, *sources,
                                   "-lm", "-o", str(work / "host")], capture_output=True, text=True)
            row["host"] = (subprocess.run([str(work / "host")], capture_output=True, text=True).stdout.strip()
                           if host.returncode == 0 else "build failed")
            image = work / "image.o"
            run, options = build(args.llvm_bin, level, defines, sources, image)
            (work / "build.log").write_text(run.stdout + run.stderr)
            row["memory_option"] = " ".join(options) or None
            if run.returncode:
                row.update(squall="compile error",
                           detail=[l for l in run.stderr.splitlines() if "error" in l][:3])
            else:
                directory = work / "image"
                load_image(image, directory, [], objcopy=str(args.llvm_bin / "llvm-objcopy"),
                           nm=str(args.llvm_bin / "llvm-nm"))
                manifest = json.loads((directory / "manifest.json").read_text())
                manifest["max_cycles"] = 2_000_000_000  # Benchmarks run for millions of tokens.
                (directory / "manifest.json").write_text(json.dumps(manifest))
                start = time.time()
                result = run_image(directory)
                row.update(tokens=result["cycles"], seconds=round(time.time() - start, 1),
                           squall="pass" if (result["status"], result["result"]) == (1, 1) else
                           f"status {result['status']} result {result['result']}")
                if args.isacomp_dir and manifest["memory_bytes"] > 512 * 1024:
                    row["rtl"] = "skipped: the RTL harness takes images up to 512 KiB"
                elif args.isacomp_dir:
                    from squall_tools.rtl import run_rtl
                    row["rtl"] = {}
                    for variant, latency in [("baseline", 0), ("pipeline", 0), ("production", 5)]:
                        try:
                            r = run_rtl(directory, isacomp=args.isacomp_dir, variant=variant,
                                        memory_latency=latency, timeout=args.rtl_timeout)
                            row["rtl"][variant] = "pass" if (r["status"], r["result"]) == (1, 1) else "fail"
                        except RuntimeError as exc:
                            row["rtl"][variant] = f"error: {str(exc)[:200]}"
            # A run passes only if the host build, the interpreter, and every
            # RTL core that ran agree that the benchmark verified.
            rtl = row.get("rtl")
            row["verdict"] = ("pass" if row["host"] == "1" and row["squall"] == "pass" and
                              (not isinstance(rtl, dict) or all(v == "pass" for v in rtl.values()))
                              else "fail")
            rows.append(row)
            print(json.dumps(row), flush=True)
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    passed = sum(r["verdict"] == "pass" for r in rows)
    skipped = sum(isinstance(r.get("rtl"), str) for r in rows)
    print(f"{passed} of {len(rows)} benchmark runs passed" +
          (f" ({skipped} too large for the RTL harness ran on the interpreter only)" if skipped else ""))
    return 0 if passed == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
