"""Build and run the pinned upstream Zig interpreter on compiler output."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys

REFERENCE_URL = "https://github.com/Lyssia-Seiden/squall-v2-reference-impl.git"
REFERENCE_COMMIT = "03dcc01ffd68abe5cf6169bd2af6090e0f398077"


def checked(command, **kwargs):
    try:
        result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"command timed out after {exc.timeout}s: {command[0]}") from exc
    if result.returncode:
        raise RuntimeError(f"command failed: {' '.join(map(str, command))}\n{result.stderr[-6000:]}")
    return result.stdout.strip()


def reference_source(directory=None):
    directory = directory or os.environ.get("SQUALL_REFERENCE_DIR")
    if directory is None:
        directory = Path.home() / ".cache/squall-tools" / REFERENCE_COMMIT
        if not directory.exists():
            directory.parent.mkdir(parents=True, exist_ok=True)
            checked(["git", "init", str(directory)])
            checked(["git", "-C", str(directory), "fetch", "--depth", "1", REFERENCE_URL, REFERENCE_COMMIT], timeout=120)
            checked(["git", "-C", str(directory), "checkout", "--detach", "FETCH_HEAD"])
    directory = Path(directory).resolve()
    revision = checked(["git", "-C", str(directory), "rev-parse", "HEAD"])
    if revision != REFERENCE_COMMIT:
        raise RuntimeError(f"reference must be pinned to {REFERENCE_COMMIT}; got {revision}")
    checked(["git", "-C", str(directory), "diff", "--exit-code", "HEAD", "--", "src/root.zig", "src/assembler.zig"])
    return directory


def zig_executable():
    zig = shutil.which("zig")
    if zig:
        return zig
    try:
        import ziglang
        return str(Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig"))
    except ImportError as exc:
        raise RuntimeError("install the test extra (ziglang==0.16.0) or put Zig 0.16.0 on PATH") from exc


def build_reference(directory=None, cache=None):
    directory = reference_source(directory)
    zig = zig_executable()
    version = checked([zig, "version"])
    if version != "0.16.0":
        raise RuntimeError(f"reference runner requires Zig 0.16.0; found {version}")
    driver = Path(__file__).with_name("reference_driver.zig")
    fingerprint = hashlib.sha256(driver.read_bytes() + version.encode() + REFERENCE_COMMIT.encode()
                                 + platform.platform().encode()).hexdigest()[:16]
    cache = Path(cache or Path.home() / ".cache/squall-tools/runners") / fingerprint
    binary = cache / ("squall-reference.exe" if os.name == "nt" else "squall-reference")
    if not binary.exists():
        cache.mkdir(parents=True, exist_ok=True)
        checked([zig, "build-exe", "-lc", "-O", "ReleaseSafe", "--dep", "squall",
                 f"-Mroot={driver}", f"-Msquall={directory / 'src/root.zig'}", f"-femit-bin={binary}"], timeout=120)
    return binary


def run_image(directory, *, reference=None, cache=None, trace=False):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("format") != "squall-v2-image-1":
        raise RuntimeError("unsupported image manifest format")
    command = [str(build_reference(reference, cache)), str(directory / "memory.bin"),
               str(manifest["stack_base"]), str(manifest["stack_top"]), str(manifest["max_cycles"]),
               str(manifest["result_word"] * 8), str(manifest["status_word"] * 8), str(manifest["memory_bytes"]),
               str(manifest["stack_token_capacity"])]
    if trace:
        command.append("trace")
    output = checked(command, timeout=60)
    if trace:
        (directory / "reference.trace").write_text(output + "\n")
    result = json.loads(output.splitlines()[-1])
    value = result["result_bits"] & 0xffffffff
    result["result"] = value - (1 << 32) if value >= 1 << 31 else value
    result["reference_commit"] = REFERENCE_COMMIT
    (directory / "reference-result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--reference-dir")
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--expect", type=lambda value: int(value, 0))
    args = parser.parse_args(argv)
    try:
        result = run_image(args.image, reference=args.reference_dir, trace=args.trace)
        print(json.dumps(result, sort_keys=True))
        if not result["quiescent"] or result["status"] != 1:
            print("Squall execution did not complete successfully", file=sys.stderr)
            return 1
        if args.expect is not None and result["result"] != args.expect:
            print(f"expected {args.expect}, got {result['result']}", file=sys.stderr)
            return 1
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"squall-run: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
