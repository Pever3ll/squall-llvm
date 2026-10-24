"""Differential runner for pinned baseline, pipeline, and production Squall RTL."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from .reference import checked, run_image

ISACOMP_COMMIT = "899ee4456cabd88deb7d2ab1088ad721178dfb8b"
HPDCACHE_COMMIT = "39fabbee1fa90bd4355444c042ed7568573cbfc1"
VARIANTS = ("baseline", "pipeline", "production")


def build_simulator(isacomp=None, cache=None, *, variant="baseline", memory_latency=5):
    if variant not in VARIANTS or not 0 <= memory_latency <= 200:
        raise RuntimeError("unknown RTL variant or memory latency outside [0, 200]")
    isacomp = isacomp or os.environ.get("SQUALL_ISACOMP_DIR")
    if not isacomp:
        raise RuntimeError("set SQUALL_ISACOMP_DIR or pass --isacomp-dir for the RTL checkout")
    directory = Path(isacomp).resolve()
    revision = checked(["git", "-C", str(directory), "rev-parse", "HEAD"])
    if revision != ISACOMP_COMMIT:
        raise RuntimeError(f"RTL checkout must be pinned to {ISACOMP_COMMIT}; got {revision}")
    checked(["git", "-C", str(directory), "diff", "--exit-code", "HEAD", "--",
             "isa/squall", "core", "backends", "opt", "test", "vendor"])
    if variant == "production" and not shutil.which("verilator"):
        raise RuntimeError("install Verilator for the HPDcache production RTL")
    if variant != "production" and (not shutil.which("iverilog") or not shutil.which("vvp")):
        raise RuntimeError("install Icarus Verilog (iverilog and vvp) for RTL tests")
    vendor_revision = None
    if variant == "production":
        vendor = directory / "vendor/cv-hpdcache"
        vendor_revision = checked(["git", "-C", str(vendor), "rev-parse", "HEAD"])
        if vendor_revision != HPDCACHE_COMMIT:
            raise RuntimeError(f"HPDcache must match the pinned Isacomp submodule; got {vendor_revision}")
        checked(["git", "-C", str(vendor), "diff", "--exit-code", "HEAD"])
    helper = Path(__file__).with_name("rtl_build.py")
    simulator_tool = "verilator" if variant == "production" else "iverilog"
    tool_version = checked([simulator_tool, "--version" if variant == "production" else "-V"])
    signature = hashlib.sha256(helper.read_bytes() + ISACOMP_COMMIT.encode()
                               + str(vendor_revision).encode() + variant.encode()
                               + str(memory_latency).encode() + tool_version.encode()).hexdigest()[:16]
    destination = Path(cache or Path.home() / ".cache/squall-tools/rtl") / ISACOMP_COMMIT / signature
    metadata = destination / "simulator.json"
    if not metadata.exists():
        destination.mkdir(parents=True, exist_ok=True)
        checked([sys.executable, str(helper), str(directory), str(destination), variant,
                 str(memory_latency)], cwd=directory, timeout=600)
    result = json.loads(metadata.read_text())
    result.update(isacomp_commit=ISACOMP_COMMIT, hpdcache_commit=vendor_revision,
                  simulator_version=tool_version.splitlines()[0], build_directory=str(destination),
                  rtl_sha256=hashlib.sha256((destination / "processor.v").read_bytes()).hexdigest(),
                  testbench_sha256=hashlib.sha256((destination / "tb.v").read_bytes()).hexdigest())
    (destination / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def build_rtl(isacomp=None, cache=None):
    """Compatibility entry point returning the baseline Icarus image."""
    return Path(build_simulator(isacomp, cache)["runner"][-1])


def tokens(output):
    result = []
    for row in output.splitlines():
        if row.startswith("T "):
            token = tuple(int(value, 16) for value in row.split()[1:])
            if token[0] != 0:  # discarded halt tokens are not useful work
                result.append(token)
    return result


def memory_words(output):
    memory = {}
    for row in output.splitlines():
        if row.startswith("M "):
            _, address, value = row.split()
            try:
                memory[int(address, 16)] = int(value, 16)
            except ValueError as exc:
                raise RuntimeError(f"unknown simulation memory value: {row}") from exc
    return memory


def observable_memory(words, manifest):
    low = (manifest["stack_base"] - manifest["stack_token_capacity"] * 16) // 8
    high = (manifest["stack_base"] + 16) // 8
    once_slots = set(manifest.get("bootstrap_slots", []))
    normalized = {}
    for address, value in words.items():
        if low <= address < high:
            continue
        if address in once_slots and value >> 62 == 0:
            value = 0  # only this spent Once slot's payload is architecturally undefined
        if value:
            normalized[address] = value
    return normalized


def execution_order_matches(actual, expected, manifest):
    """Allow bootstrap argument scheduling, then require the serial ABI order."""
    entry = manifest["functions"][manifest["entry"]]["entry_word"]
    frame = manifest["root_frame_word"]
    def split(rows):
        at = next((i for i, token in enumerate(rows) if token[0] == entry and token[1] == frame), None)
        return None if at is None else (rows[:at], rows[at:])
    left, right = split(actual), split(expected)
    return (left is not None and right is not None
            and Counter(left[0]) == Counter(right[0]) and left[1] == right[1])


def run_rtl(directory, *, isacomp=None, cache=None, reference=None,
            variant="baseline", memory_latency=5, stall_seed=0, stall_jitter=0):
    if not 0 <= stall_seed < 1 << 31 or not 0 <= stall_jitter <= 200:
        raise RuntimeError("stall seed must fit positive i32 and jitter must be in [0, 200]")
    if stall_jitter and (variant != "production" or not memory_latency):
        raise RuntimeError("random stalls require production RTL with positive memory latency")
    directory = Path(directory).resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    # The pinned baseline TB dumps 65536 64-bit words. Larger images require a
    # different harness; reject rather than silently comparing partial memory.
    if manifest["memory_bytes"] > 512 * 1024:
        raise RuntimeError("baseline RTL harness supports images up to 512 KiB")
    expected = run_image(directory, reference=reference, trace=True)
    if not expected["quiescent"] or expected["status"] != 1:
        raise RuntimeError(f"reference execution failed: {expected}")
    simulator = build_simulator(isacomp, cache, variant=variant, memory_latency=memory_latency)
    # The upstream testbench has a 128-byte filename register. Running in the
    # image directory also avoids truncation of long absolute workspace paths.
    command = [*simulator["runner"], "+memory_file=memory.hex",
                      f"+stack_base={manifest['stack_base']}", f"+stack_top={manifest['stack_top']}",
                      f"+stack_low={manifest['stack_base'] - manifest['stack_token_capacity'] * 16}",
                      f"+max_tokens={manifest['max_cycles']}", "+max_idle=5000",
                      f"+pipeline_length={simulator['pipeline_length']}",
                      f"+stall_seed={stall_seed}", f"+stall_jitter={stall_jitter}"]
    stem = "rtl" if variant == "baseline" else f"rtl-{variant}-lat{memory_latency}"
    if stall_jitter:
        stem += f"-seed{stall_seed}-jitter{stall_jitter}"
    trace_path = directory / f"{stem}.trace"
    try:
        simulation = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or b""
        trace_path.write_text(partial.decode(errors="replace") if isinstance(partial, bytes) else partial)
        raise RuntimeError(f"RTL host timeout; inspect {trace_path}") from exc
    output = simulation.stdout + simulation.stderr
    (directory / f"{stem}.trace").write_text(output + "\n")
    if (simulation.returncode or "DRAINED" not in output.splitlines() or "SIM_COMPLETE" not in output.splitlines()
            or "ERROR:" in output or "FATAL:" in output or "%Error" in output):
        raise RuntimeError(f"RTL did not drain cleanly; inspect {trace_path}")
    memory = memory_words(output)
    value = memory.get(manifest["result_word"], 0) & 0xffffffff
    actual = value - (1 << 32) if value >= 1 << 31 else value
    reference_tokens = tokens((directory / "reference.trace").read_text())
    rtl_tokens = tokens(output)
    reference_memory = observable_memory(memory_words((directory / "reference.trace").read_text()), manifest)
    rtl_memory = observable_memory(memory, manifest)
    differences = [{"word": address, "expected": reference_memory.get(address, 0),
                    "actual": rtl_memory.get(address, 0)}
                   for address in sorted(reference_memory.keys() | rtl_memory.keys())
                   if reference_memory.get(address, 0) != rtl_memory.get(address, 0)]
    result = {"result": actual, "status": memory.get(manifest["status_word"], 0),
              "tokens": len(rtl_tokens), "trace_order_matches": rtl_tokens == reference_tokens,
              "trace_multiset_matches": Counter(rtl_tokens) == Counter(reference_tokens),
              "execution_order_matches": execution_order_matches(rtl_tokens, reference_tokens, manifest),
              "memory_matches": not differences, "memory_differences": differences[:20],
              "isacomp_commit": ISACOMP_COMMIT, "reference_commit": expected["reference_commit"],
              "simulator": simulator, "stall_seed": stall_seed, "stall_jitter": stall_jitter}
    result["trace_matches"] = result["trace_order_matches" if variant == "baseline" else "execution_order_matches"]
    result["wall_cycles"] = next((int(row.split()[1]) for row in output.splitlines() if row.startswith("WALL ")), None)
    (directory / f"{stem}-result.json").write_text(json.dumps(result, indent=2) + "\n")
    if actual != expected["result"] or result["status"] != 1 or not result["trace_matches"] or differences:
        raise RuntimeError(f"RTL/reference mismatch: {result}; inspect {trace_path} and reference.trace")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--isacomp-dir")
    parser.add_argument("--reference-dir")
    parser.add_argument("--variant", choices=VARIANTS, default="baseline")
    parser.add_argument("--memory-latency", type=int, default=5)
    parser.add_argument("--stall-seed", type=int, default=0)
    parser.add_argument("--stall-jitter", type=int, default=0)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(run_rtl(args.image, isacomp=args.isacomp_dir, reference=args.reference_dir,
                                 variant=args.variant, memory_latency=args.memory_latency,
                                 stall_seed=args.stall_seed, stall_jitter=args.stall_jitter), sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"squall-rtl: {exc}", file=sys.stderr)
        return 1
