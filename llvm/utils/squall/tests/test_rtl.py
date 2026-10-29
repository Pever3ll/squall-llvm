"""llc-generated images executed on the generated Isacomp Squall RTL."""
import os

import pytest

import json
import subprocess

from conftest import EXAMPLES, clang, compile_ir, load, requires_llvm
from squall_tools.rtl import run_rtl
from test_codegen import FIBONACCI, LANE_GUARD

requires_rtl = pytest.mark.skipif(not os.environ.get("SQUALL_ISACOMP_DIR"),
                                  reason="set SQUALL_ISACOMP_DIR to the pinned RTL checkout")

PROGRAMS = {
    "add": ("define i32 @main(i32 %a, i32 %b) { %r = add i32 %a, %b\nret i32 %r }", [7, 35], 42),
    "fibonacci": (FIBONACCI, [8], 21),
    "memory": ("""@a = global [8 x i32] [i32 2, i32 -3, i32 5, i32 7, i32 11, i32 -13, i32 17, i32 19]
      define i32 @sum(ptr %p, i32 %n) noinline {
      entry: br label %loop
      loop: %i = phi i32 [0, %entry], [%next, %loop]
        %s = phi i32 [0, %entry], [%t, %loop]
        %q = getelementptr i32, ptr %p, i32 %i
        %v = load i32, ptr %q
        %t = add i32 %s, %v
        store i32 %t, ptr %q
        %next = add i32 %i, 1
        %c = icmp slt i32 %next, %n
        br i1 %c, label %loop, label %done
      done: ret i32 %t
      }
      define i32 @main(i32 %n) {
        %r = call i32 @sum(ptr @a, i32 %n)
        ret i32 %r
      }""", [8], 45),
}


@pytest.mark.parametrize("options,error", [
    ({"stall_seed": -1}, "stall seed"),
    ({"stall_jitter": 201}, "jitter"),
    ({"stall_jitter": 1}, "require production"),
    ({"variant": "production", "memory_latency": 0, "stall_jitter": 1}, "positive memory latency"),
])
def test_invalid_random_stalls_fail_before_execution(options, error):
    with pytest.raises(RuntimeError, match=error):
        run_rtl("does-not-exist", **options)


@requires_llvm
@requires_rtl
@pytest.mark.parametrize("variant", ["baseline", "pipeline", "production"])
@pytest.mark.parametrize("program", list(PROGRAMS))
def test_programs_on_rtl(tmp_path, program, variant):
    ir, arguments, expected = PROGRAMS[program]
    image = compile_ir(tmp_path, ir, arguments, opt="-O2")
    result = run_rtl(image, variant=variant, memory_latency=5)
    assert result["result"] == expected
    assert result["trace_matches"] and result["memory_matches"], result


@requires_llvm
@requires_rtl
def test_seeded_stalls_repeat_and_change_timing(tmp_path):
    image = compile_ir(tmp_path, "define i32 @main(i32 %a) { %r = mul i32 %a, -7\nret i32 %r }", [6])
    options = {"variant": "production", "memory_latency": 5, "stall_jitter": 19}
    first = run_rtl(image, stall_seed=17, **options)
    repeat = run_rtl(image, stall_seed=17, **options)
    other = run_rtl(image, stall_seed=91, **options)
    fixed = run_rtl(image, variant="production", memory_latency=5)
    assert first["result"] == repeat["result"] == other["result"] == fixed["result"] == -42
    assert first["wall_cycles"] == repeat["wall_cycles"]
    assert first["wall_cycles"] != other["wall_cycles"]
    assert first["wall_cycles"] > fixed["wall_cycles"]


@requires_llvm
@requires_rtl
@pytest.mark.parametrize("variant,latency", [("baseline", 0), ("pipeline", 0), ("production", 5)])
def test_fork_join_lanes_overlap_on_rtl(tmp_path, variant, latency):
    """Two independent chains per iteration run as two tokens: same result and
    executed tokens on every core, fewer cycles where tokens overlap."""
    if not clang():
        pytest.skip("the built Squall Clang is required")
    cycles = {}
    for fork in (False, True):
        obj = tmp_path / f"chains{int(fork)}.o"
        subprocess.run([clang(), "--target=squallv2", "-O2", "-c", "-mllvm", "-squall-entry=chains",
                        *(["-mllvm", "-squall-fork-join"] if fork else []),
                        str(EXAMPLES / "parallel.c"), "-o", str(obj)], check=True)
        image = tmp_path / f"image{int(fork)}"
        load(obj, image, [60])
        assert json.loads((image / "manifest.json").read_text())["parallel_lanes"] == fork
        result = run_rtl(image, variant=variant, memory_latency=latency)
        assert result["trace_matches"] and result["memory_matches"], result
        cycles[fork] = result["wall_cycles"]
    if variant != "baseline":
        assert cycles[True] < cycles[False] * 0.8, cycles


@requires_llvm
@requires_rtl
@pytest.mark.parametrize("variant,latency", [("baseline", 0), ("pipeline", 0), ("production", 5)])
@pytest.mark.parametrize("i,status", [(2, 1), (65536, 3)])
def test_memory_guard_inside_parallel_lane_on_rtl(tmp_path, variant, latency, i, status):
    image = compile_ir(tmp_path, LANE_GUARD, [i, 77], opt="-O2", options=["-squall-fork-join"])
    result = run_rtl(image, variant=variant, memory_latency=latency, expected_status=status)
    assert result["status"] == status, result
    assert result["trace_matches"] and result["memory_matches"], result


@requires_llvm
@requires_rtl
@pytest.mark.parametrize("variant,latency", [("baseline", 0), ("pipeline", 0), ("production", 5)])
def test_call_in_a_lane_on_rtl(tmp_path, variant, latency):
    """A call runs in one lane while the other computes; the region joins on
    a reserved word, and the callee's own lanes join on the shared one."""
    if not clang():
        pytest.skip("the built Squall Clang is required")
    obj = tmp_path / "call.o"
    subprocess.run([clang(), "--target=squallv2", "-O2", "-c", "-mllvm", "-squall-entry=call_beside_arithmetic",
                    "-mllvm", "-squall-fork-join", str(EXAMPLES / "parallel.c"), "-o", str(obj)], check=True)
    image = tmp_path / "image"
    load(obj, image, [27])
    manifest = json.loads((image / "manifest.json").read_text())
    assert manifest["parallel_lanes"] and 16 in manifest["join_words"], manifest["join_words"]
    result = run_rtl(image, variant=variant, memory_latency=latency)
    assert result["trace_matches"] and result["memory_matches"], result
