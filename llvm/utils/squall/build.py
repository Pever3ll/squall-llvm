#!/usr/bin/env python3
"""Restore omitted pinned sources, then configure, build and check LLVM/Clang
with the Squall backend."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
UPSTREAM = "85ac560262434c9ccfc0c183ec22d4138ed647fb"  # llvmorg-23.1.2
RAW = f"https://raw.githubusercontent.com/llvm/llvm-project/{UPSTREAM}/"
TEST_INFRASTRUCTURE = (
    "llvm/test/CMakeLists.txt", "llvm/test/lit.cfg.py", "llvm/test/lit.site.cfg.py.in",
    "llvm/test/Unit/CMakeLists.txt", "llvm/test/Unit/lit.cfg.py", "llvm/test/Unit/lit.site.cfg.py.in",
    "llvm/test/FileCheck/CMakeLists.txt", "llvm/test/TableGen/CMakeLists.txt",
)
TEST_SELECTION = {"Assembler", "Verifier", "FileCheck", "TableGen", "Unit"}


def run(command, **options):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **options)


def restore():
    digest = hashlib.sha256((ROOT / "llvm/lib/Support/APFloat.cpp").read_bytes()).hexdigest()
    if digest != "199e7d48a3ffaacbe1cdf85068aada7aef822272383857fff29d2916e024f5b4":
        raise RuntimeError("APFloat.cpp no longer matches the pinned LLVM 23.1.2 source; review the source pin")
    needs_libc = not ((ROOT / "libc/shared/math.h").exists()
                      and (ROOT / "libc/UPSTREAM-SOURCE.json").exists())
    needs_tests = not (ROOT / "llvm/test/UPSTREAM-SOURCE.json").exists()
    if needs_libc or needs_tests:
        url = f"https://codeload.github.com/llvm/llvm-project/tar.gz/{UPSTREAM}"
        print(f"Restoring omitted sources from {UPSTREAM}", flush=True)
        counts = {"libc": 0, "tests": 0}
        with urllib.request.urlopen(url, timeout=120) as response:
            with tarfile.open(fileobj=response, mode="r|gz") as archive:
                for member in archive:
                    parts = Path(member.name).parts
                    if len(parts) < 3 or not member.isfile():
                        continue
                    component = None
                    if needs_libc and parts[1] == "libc":
                        component = "libc"
                    elif (needs_tests and len(parts) >= 4 and parts[1:3] == ("llvm", "test")
                          and (len(parts) == 4 or parts[3] in TEST_SELECTION)):
                        component = "tests"
                    if component is None:
                        continue
                    target = (ROOT / Path(*parts[1:])).resolve()
                    if not target.is_relative_to(ROOT):
                        raise RuntimeError("invalid upstream archive path")
                    data = archive.extractfile(member).read()
                    if target.exists() and target.read_bytes() != data:
                        raise RuntimeError(f"refusing to overwrite modified source: {target}")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    counts[component] += 1
        provenance = {"repository": "https://github.com/llvm/llvm-project",
                      "tag": "llvmorg-23.1.2", "commit": UPSTREAM}
        if needs_libc:
            (ROOT / "libc/UPSTREAM-SOURCE.json").write_text(json.dumps(dict(provenance,
                reason="Restore omitted libc source required by LLVMSupport/APFloat.cpp",
                files=counts["libc"]), indent=2) + "\n")
        if needs_tests:
            (ROOT / "llvm/test/UPSTREAM-SOURCE.json").write_text(json.dumps(dict(provenance,
                selection=["top-level infrastructure", *sorted(TEST_SELECTION)], files=counts["tests"],
                note="Other LLVM regression subdirectories were omitted by the original repository import and have not been restored."),
                indent=2) + "\n")
    for relative in TEST_INFRASTRUCTURE:
        target = ROOT / relative
        if not target.exists():
            data = urllib.request.urlopen(RAW + relative, timeout=60).read()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)


def verify(build):
    binary = build / "bin"
    run([binary / "llvm-config", "--version", "--targets-built"])
    run([binary / "clang", "--version"])
    run([build / "unittests/TargetParser/TargetParserTests"])
    run([sys.executable, build / "bin/llvm-lit", "-v",
         ROOT / "clang/test/Preprocessor/squall.c", ROOT / "clang/test/CodeGen/squall.c",
         ROOT / "clang/test/Driver/squall-toolchain.c",
         ROOT / "llvm/test/CodeGen/Squall", ROOT / "llvm/test/MC/Squall"])
    run([sys.executable, build / "bin/llvm-lit", "-q", "-j", "4",
         ROOT / "llvm/test/Assembler", ROOT / "llvm/test/Verifier",
         ROOT / "llvm/test/FileCheck", ROOT / "llvm/test/TableGen"])
    smoke = build / "squall-smoke"
    smoke.mkdir(exist_ok=True)
    run([binary / "clang", "--target=squallv2", "-O2", "-c", "-mllvm", "-squall-entry=kernel",
         Path(__file__).with_name("examples") / "kernel.c", "-o", smoke / "kernel.o"])
    run([binary / "llvm-readobj", "--sections", smoke / "kernel.o"], stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT.parent / "squall-llvm-build")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--llvm-only", action="store_true", help="Build LLVM tools without the Clang project")
    parser.add_argument("--static-tools", action="store_true", help="Link only requested tool dependencies instead of the LLVM dylib")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--target", action="append", default=[])
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    ninja = shutil.which("ninja")
    if not ninja:
        raise RuntimeError("install Ninja (python -m pip install ninja), then put it on PATH")
    restore()
    build = args.build_dir.resolve()
    configure = ["cmake", "-S", ROOT / "llvm", "-B", build, "-G", "Ninja", "-C",
                 Path(__file__).with_name("native-cache.cmake"), f"-DCMAKE_MAKE_PROGRAM={ninja}",
                 f"-DPython3_EXECUTABLE={sys.executable}"]
    if args.llvm_only:
        configure.append("-DLLVM_ENABLE_PROJECTS=")
    if args.static_tools:
        configure.extend(["-DLLVM_BUILD_LLVM_DYLIB=OFF", "-DLLVM_LINK_LLVM_DYLIB=OFF"])
    if args.check and args.llvm_only:
        parser.error("--check includes Clang; omit --llvm-only or run the LLVM checks separately")
    run(configure)
    if args.configure_only:
        return
    command = ["cmake", "--build", build, "-j", str(args.jobs)]
    if args.target:
        command += ["--target", *args.target]
    run(command)
    if args.check:
        run(["cmake", "--build", build, "--target", "TargetParserTests", "-j", str(args.jobs)])
        verify(build)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Squall LLVM build failed: {exc}", file=sys.stderr)
        sys.exit(1)
