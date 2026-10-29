"""The Squall runtime: the C library, software floating point, and traps.

Programs are linked rather than compiled with -c, so the driver adds the
runtime bitcode; each is compared with the same source built for the host
against the host's C library and floating-point hardware."""
import ctypes
import json
import random
import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import EXAMPLES, clang, host_clang, i32, load, requires_llvm
from squall_tools.reference import run_image

pytestmark = requires_llvm

LIBC = {
    "strings": [(0,), (1,), (5,), (26,), (39,), (-17,)],
    "search": [(i,) for i in range(6)],
    "parse": [(i,) for i in range(12)],
    "sorting": [(0,), (1,), (12345,), (-99,)],
    "classes": [(c,) for c in (0, 9, 10, 32, 48, 57, 65, 70, 90, 95, 97, 102, 122, 126, 127)],
    "arithmetic": [(0,), (1,), (-7,), (100,), (-123457,)],
    "checked": [(0,), (21,)],
}
FLOATS = {
    "float_arithmetic": [(0, 0), (1, 2), (-7, 3), (1000, -999), (123456, 7)],
    "double_arithmetic": [(0, 0), (1, 2), (-7, 3), (1000, -999), (123456, 7)],
    "comparisons": [(1, 1), (1, 2), (2, 1), (1000, 5), (0, 1000), (1000, 1000), (-3, -3)],
    "conversions": [(0,), (1,), (-1,), (7,), (-12345,), (999999,)],
    "math": [(0,), (1,), (-1,), (7,), (-7,), (10,), (-35,), (123456,)],
    "newton": [(0,), (2,), (12345,), (65533,)],
    "globals": [(0,), (1,), (2,), (3,)],
    "specials": [(i,) for i in range(8)],
}


FRAMES = {
    "variadic.c": {"variadic": [(0,), (1,), (-5,), (1000,)], "variadic_pointer": [(0,), (9,)]},
    "vla.c": {"vla_sum": [(0,), (7,), (-59,)], "vla_loop": [(0,), (5,), (15,)],
              "vla_recursive": [(0,), (3,), (7,)], "alloca_bytes": [(0,), (12,), (31,)],
              "vla_overflow": [(1,), (100,)]},
}


def runtime_available():
    return clang() and (Path(clang()).resolve().parent.parent / "lib/clang-runtimes/squallv2/lib/libc.bc").exists()


requires_runtime = pytest.mark.skipif(not runtime_available(), reason="the Squall runtime is not built")


def host_library(tmp_path, source):
    library = tmp_path / "host.so"
    subprocess.run([host_clang(), "-O0", "-ffp-contract=off", "-shared", "-fPIC", str(source),
                    "-lm", "-o", str(library)], check=True)
    return library


def host_call(tmp_path, library, entry, arguments, index):
    copy = tmp_path / f"host-{entry}-{index}.so"
    shutil.copy(library, copy)
    function = getattr(ctypes.CDLL(str(copy)), entry)
    function.argtypes, function.restype = [ctypes.c_int32] * len(arguments), ctypes.c_uint32
    return i32(function(*arguments))


def link(tmp_path, sources, entry, level, options=()):
    image = tmp_path / f"{entry}{level}.o"
    subprocess.run([clang(), "--target=squallv2", level, "-mllvm", f"-squall-entry={entry}", *options,
                    *map(str, sources), "-o", str(image)], check=True, capture_output=True, text=True)
    return image


def check_against_host(tmp_path, source, cases, level, options=()):
    library = host_library(tmp_path, source)
    for entry, inputs in cases.items():
        image = link(tmp_path, [source], entry, level, options)
        for index, arguments in enumerate(inputs):
            expected = host_call(tmp_path, library, entry, arguments, index)
            load(image, tmp_path / f"{entry}-{index}", arguments)
            result = run_image(tmp_path / f"{entry}-{index}")
            assert result["status"] == 1 and result["result"] == expected, (entry, arguments, result)


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2"])
def test_c_library_matches_host(tmp_path, level):
    check_against_host(tmp_path, EXAMPLES / "libc.c", LIBC, level)


@requires_runtime
@pytest.mark.parametrize("options", [(), ("-mllvm", "-squall-fork-join")], ids=["serial", "fork-join"])
@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2"])
def test_floating_point_matches_host(tmp_path, level, options):
    check_against_host(tmp_path, EXAMPLES / "floats.c", FLOATS, level, options)


@requires_runtime
@pytest.mark.parametrize("options", [(), ("-mllvm", "-squall-fork-join")], ids=["serial", "fork-join"])
@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2"])
@pytest.mark.parametrize("source", list(FRAMES))
def test_variadic_functions_and_dynamic_allocation(tmp_path, source, level, options):
    check_against_host(tmp_path, EXAMPLES / source, FRAMES[source], level, options)


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_exhausted_alloca_area_traps(tmp_path, level):
    image = link(tmp_path, [EXAMPLES / "vla.c"], "vla_overflow", level)
    load(image, tmp_path / "run", [2000])  # 8000 bytes; the area holds 4096.
    result = run_image(tmp_path / "run")
    assert result["quiescent"] and result["status"] == 4, result
    image = link(tmp_path, [EXAMPLES / "vla.c"], "vla_overflow", level, ("-mllvm", "-squall-alloca-bytes=16384"))
    load(image, tmp_path / "larger", [2000])
    result = run_image(tmp_path / "larger")
    assert result["status"] == 1 and result["result"] == 2002, result


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_failed_assertion_traps(tmp_path, level):
    image = link(tmp_path, [EXAMPLES / "libc.c"], "checked", level)
    load(image, tmp_path / "run", [-1])
    result = run_image(tmp_path / "run")
    manifest = json.loads((tmp_path / "run/manifest.json").read_text())
    assert result["quiescent"] and result["status"] == manifest["status_codes"]["trap"] == 4, result


@requires_runtime
def test_unused_runtime_is_not_in_the_image(tmp_path):
    """Only what the program calls reaches the image: a program without
    floating point or library calls is no larger than without the runtime."""
    source = tmp_path / "plain.c"
    source.write_text("int main(int x) { return x * 3 + 1; }\n")
    sizes = {}
    for name, options in [("runtime", ()), ("bare", ("-nostdlib",))]:
        image = link(tmp_path, [source], "main", "-O2", options)
        load(image, tmp_path / name, [5])
        assert run_image(tmp_path / name)["result"] == 16
        sizes[name] = json.loads((tmp_path / name / "manifest.json").read_text())["code_start_word"], \
            image.stat().st_size
    assert sizes["runtime"] == sizes["bare"]


@requires_runtime
def test_float_needs_the_linked_runtime(tmp_path):
    source = tmp_path / "f.c"
    source.write_text("int main(int x) { return (int)((float)x * 1.5f); }\n")
    run = subprocess.run([clang(), "--target=squallv2", "-c", str(source), "-o", str(tmp_path / "f.o")],
                         capture_output=True, text=True)
    assert run.returncode != 0
    assert "'__mulsf3' has no definition in this program; the Squall runtime library is linked only " \
           "when clang links the program, without -c" in run.stderr


# Random operands for every arithmetic operation, conversion, and comparison,
# including subnormals, infinities, and NaN, compared bit for bit.
OPERATIONS = """
#include <string.h>
typedef unsigned u32;
typedef unsigned long long u64;
static float f(u32 u) { float x; memcpy(&x, &u, 4); return x; }
static u32 fb(float x) { u32 u; memcpy(&u, &x, 4); return x != x ? 0x7fc00000u : u; }
static double d(u32 hi, u32 lo) { u64 u = (u64)hi << 32 | lo; double x; memcpy(&x, &u, 8); return x; }
static u32 db(double x) { u64 u; memcpy(&u, &x, 8); return x != x ? 0x7ff80000u : (u32)(u >> 32) ^ (u32)u * 7u; }
int fop(int op, int a, int b) {
  float x = f((u32)a), y = f((u32)b);
  switch (op) {
  case 0: return (int)fb(x + y);
  case 1: return (int)fb(x - y);
  case 2: return (int)fb(x * y);
  case 3: return (int)fb(x / y);
  case 4: return (x < y) | (x <= y) << 1 | (x > y) << 2 | (x >= y) << 3 | (x == y) << 4 | (x != y) << 5;
  case 5: return x != x || x > 2e9f || x < -2e9f ? 0 : (int)x;
  case 6: return (int)db((double)x);
  default: return (int)fb((float)a) ^ (int)fb((float)(u32)b);
  }
}
int dop(int op, int a, int b, int c, int e) {
  double x = d((u32)a, (u32)b), y = d((u32)c, (u32)e);
  switch (op) {
  case 0: return (int)db(x + y);
  case 1: return (int)db(x - y);
  case 2: return (int)db(x * y);
  case 3: return (int)db(x / y);
  case 4: return (x < y) | (x <= y) << 1 | (x > y) << 2 | (x >= y) << 3 | (x == y) << 4 | (x != y) << 5;
  case 5: return x != x || x > 9e18 || x < -9e18 ? 0 : (int)((long long)x ^ (long long)x >> 32);
  case 6: return (int)fb((float)x);
  default: { u64 v = (u64)(u32)a << 32 | (u32)b; return (int)db((double)(long long)v) ^ (int)db((double)v); }
  }
}
"""


def interesting_float(rng):
    choice = rng.randrange(6)
    if choice == 0:
        return rng.choice([0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x00000001, 0x007fffff,
                           0x00800000, 0x7f7fffff, 0x3f800000, 0xbf800000, 0x3effffff])
    if choice == 1:
        return rng.randrange(1 << 23) | rng.choice([0, 0x80000000])  # Subnormal.
    if choice == 2:
        return (rng.randrange(118, 137) << 23 | rng.randrange(1 << 23)) | rng.choice([0, 0x80000000])
    return rng.randrange(1 << 32)


def interesting_double(rng):
    choice = rng.randrange(6)
    if choice == 0:
        return rng.choice([0, 1 << 63, 0x7ff << 52, 0xfff << 52, 0x7ff8 << 48, 1, (1 << 52) - 1, 1 << 52,
                           0x7fefffffffffffff, 0x3ff << 52, 0xbff << 52])
    if choice == 1:
        return rng.randrange(1 << 52) | rng.choice([0, 1 << 63])
    if choice == 2:
        return (rng.randrange(1013, 1034) << 52 | rng.randrange(1 << 52)) | rng.choice([0, 1 << 63])
    return rng.randrange(1 << 64)


@requires_runtime
@pytest.mark.parametrize("seed", range(3))
def test_random_operations_match_host(tmp_path, seed):
    source = tmp_path / "operations.c"
    source.write_text(OPERATIONS)
    library = host_library(tmp_path, source)
    rng = random.Random(seed)
    cases = {"fop": [], "dop": []}
    for op in range(8):
        for _ in range(6):
            cases["fop"].append((op, i32(interesting_float(rng)), i32(interesting_float(rng))))
            x, y = interesting_double(rng), interesting_double(rng)
            cases["dop"].append((op, i32(x >> 32), i32(x), i32(y >> 32), i32(y)))
    check_against_host(tmp_path, source, cases, "-O2")
