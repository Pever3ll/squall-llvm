"""The Squall runtime: the C library, software floating point, and traps.

Programs are linked rather than compiled with -c, so the driver adds the
runtime bitcode; each is compared with the same source built for the host
against the host's C library and floating-point hardware, except cbrt,
which is checked against the exact cube root."""
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
    "unsigned_limits": [(i,) for i in range(6)],
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
@pytest.mark.parametrize("text,value,overflow", [
    ("-4294967296", 0xFFFFFFFF, True), ("-99999999999", 0xFFFFFFFF, True),
    ("4294967296", 0xFFFFFFFF, True), ("-4294967295", 1, False), ("-1", 0xFFFFFFFF, False),
])
def test_strtoul_saturates_on_overflow(tmp_path, text, value, overflow):
    """long is 32 bits on Squall (64 on most hosts), so strtoul is checked
    against fixed values rather than the host."""
    source = tmp_path / "strtoul.c"
    source.write_text(f"""#include <errno.h>
#include <stdlib.h>
int main(void) {{
  errno = 0;
  unsigned long v = strtoul("{text}", 0, 10);
  return (v == {value}u) + 2 * (errno == ERANGE);
}}
""")
    image = link(tmp_path, [source], "main", "-O0")
    load(image, tmp_path / "run")
    result = run_image(tmp_path / "run")
    assert result["status"] == 1 and result["result"] == 1 + 2 * overflow, result


MATH_INPUTS = [0, 1, -1, 7, -13, 64, -200, 511, 1700, -2999, 1000, 1001, 1002, 1003]


def ulps_apart(a, b, width):
    """Distance in ulps between two IEEE encodings; NaNs of any payload are
    equal, and only the same infinity or zero matches."""
    import math
    import struct
    fmt, ints = ("<d", "<Q") if width == 64 else ("<f", "<I")
    x, y = (struct.unpack(fmt, struct.pack(ints, v & ((1 << width) - 1)))[0] for v in (a, b))
    if math.isnan(x) or math.isnan(y):
        return 0 if math.isnan(x) and math.isnan(y) else 1 << 62
    if math.isinf(x) or math.isinf(y) or x == 0 or y == 0:
        return 0 if (a ^ b) & ((1 << width) - 1) == 0 else 1 << 62
    order = lambda v: v if v >> (width - 1) == 0 else -(v & ((1 << (width - 1)) - 1))
    return abs(order(a & ((1 << width) - 1)) - order(b & ((1 << width) - 1)))


def assert_cbrt_within_one_ulp(bits, a, width):
    """Bracket cbrt(a / 64) with IEEE values using exact rational cubes."""
    from fractions import Fraction
    import struct
    fraction_bits = 52 if width == 64 else 23
    sign = 1 << (width - 1)
    infinity = ((1 << (width - fraction_bits - 1)) - 1) << fraction_bits
    # libm.c maps 1000..1003 to infinity, -infinity, NaN, and -0.0.
    special = {0: 0, 1000: infinity, 1001: sign | infinity,
               1002: infinity | 1, 1003: sign}
    if a in special:
        assert ulps_apart(bits, special[a], width) == 0, (13, a, hex(bits), hex(special[a]))
        return
    fmt, ints = ("<d", "<Q") if width == 64 else ("<f", "<I")

    def cube(encoding):
        return Fraction(struct.unpack(fmt, struct.pack(ints, encoding))[0]) ** 3

    target = Fraction(abs(a), 64)
    lower, upper = 0, infinity
    while upper - lower > 1:
        middle = (lower + upper) // 2
        if cube(middle) <= target:
            lower = middle
        else:
            upper = middle
    # An exactly representable root also permits either immediate neighbor.
    if cube(lower) == target:
        lower, upper = lower - 1, lower + 1
    if a < 0:
        lower, upper = lower | sign, upper | sign
    assert lower <= bits <= upper, (13, a, hex(bits), (hex(lower), hex(upper)))


def test_cbrt_exact_oracle():
    assert_cbrt_within_one_ulp(0xBFE2CF888F8DB02F, -13, 64)
    with pytest.raises(AssertionError):
        assert_cbrt_within_one_ulp(0xBFE2CF888F8DB02D, -13, 64)
    for width, one in ((32, 0x3F800000), (64, 0x3FF0000000000000)):
        for bits in (one - 1, one, one + 1):
            assert_cbrt_within_one_ulp(bits, 64, width)
        with pytest.raises(AssertionError):
            assert_cbrt_within_one_ulp(one - 2, 64, width)


@requires_runtime
@pytest.mark.parametrize("op", range(20))
def test_math_functions_within_an_ulp_of_host(tmp_path, op):
    """LLVM libc's implementations, through Squall's soft float, against the
    exact cube root for cbrt and the host's libm otherwise: at most an ulp
    apart, and the same on special values."""
    source = EXAMPLES / "libm.c"
    library = host_library(tmp_path, source) if op != 13 else None
    images = {entry: link(tmp_path, [source], entry, "-O2", ("-mllvm", "-squall-memory-bytes=2097152"))
              for entry in ("math_high", "math_low", "mathf_bits")}
    for index, a in enumerate(MATH_INPUTS):
        got = {}
        for entry, image in images.items():
            load(image, tmp_path / f"{entry}-{index}", [op, a])
            result = run_image(tmp_path / f"{entry}-{index}")
            assert result["status"] == 1, (entry, op, a, result)
            got[entry] = result["result"] & 0xFFFFFFFF
        double = lambda r: r["math_high"] << 32 | r["math_low"]
        if op == 13:
            # Some host libms are several ulps off for cbrt.
            assert_cbrt_within_one_ulp(double(got), a, 64)
            assert_cbrt_within_one_ulp(got["mathf_bits"], a, 32)
        else:
            want = {entry: host_call(tmp_path, library, entry, (op, a), f"{op}-{index}") & 0xFFFFFFFF
                    for entry in images}
            assert ulps_apart(double(got), double(want), 64) <= 1, (op, a, hex(double(got)), hex(double(want)))
            assert ulps_apart(got["mathf_bits"], want["mathf_bits"], 32) <= 1, (
                op, a, hex(got["mathf_bits"]), hex(want["mathf_bits"]))


@requires_runtime
def test_frexp_ldexp_modf_match_host(tmp_path):
    check_against_host(tmp_path, EXAMPLES / "libm.c",
                       {"split": [(a,) for a in (0, 1, -1, 7, -13, 64, -200, 511, 1700, -2999)]}, "-O2",
                       ("-mllvm", "-squall-memory-bytes=2097152"))


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
@pytest.mark.parametrize("exponent,high", [
    ("i64 4294967296", 0x7FF00000), ("i64 -4294967296", 0), ("i64 3", 0x40200000),
    ("i16 -2", 0x3FD00000), ("i32 1024", 0x7FF00000),
])
def test_ldexp_exponents_of_every_width(tmp_path, level, exponent, high):
    """The library's ldexp takes an int; wider exponents clamp, which gives
    the same saturated result, and narrower ones sign-extend."""
    width = exponent.split()[0]
    source = tmp_path / "ldexp.ll"
    source.write_text(f"""define i32 @main() {{
  %r = call double @llvm.ldexp.f64.{width}(double 1.0, {exponent})
  %bits = bitcast double %r to i64
  %top = lshr i64 %bits, 32
  %t = trunc i64 %top to i32
  ret i32 %t
}}
declare double @llvm.ldexp.f64.{width}(double, {width})
""")
    image = link(tmp_path, [source], "main", level)
    load(image, tmp_path / "run")
    result = run_image(tmp_path / "run")
    assert result["status"] == 1 and result["result"] == i32(high), result


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_exit_ends_the_program_with_its_status(tmp_path, level):
    source = tmp_path / "exit.c"
    source.write_text("""#include <stdlib.h>
static int depth(int n, int code) {
  if (n == 0)
    exit(code);
  return depth(n - 1, code) + 1;
}
int run(int code) {
  if (code < 0)
    _Exit(-code);
  return depth(5, code * 2) + 100;
}
""")
    image = link(tmp_path, [source], "run", level)
    for index, (code, result) in enumerate([(21, 42), (-7, 7), (0, 0)]):
        load(image, tmp_path / f"run{index}", [code])
        run = run_image(tmp_path / f"run{index}")
        assert run["quiescent"] and run["status"] == 1 and run["result"] == result, (code, run)


@requires_runtime
def test_stdio_declarations_compile_and_calls_are_diagnosed(tmp_path):
    """Code that only mentions stdio on unused paths links; a live call
    names the function and the runtime."""
    quiet = tmp_path / "quiet.c"
    quiet.write_text("""#include <stdio.h>
static void trace(const char *s) { printf("%s\\n", s); }
int main(void) {
  if (0)
    trace("never");
  return 5;
}
""")
    image = link(tmp_path, [quiet], "main", "-O2")
    load(image, tmp_path / "quiet")
    assert run_image(tmp_path / "quiet")["result"] == 5
    loud = tmp_path / "loud.c"
    loud.write_text('#include <stdio.h>\nint main(void) { return printf("hi"); }\n')
    run = subprocess.run([clang(), "--target=squallv2", "-O2", str(loud), "-o", str(tmp_path / "loud.o")],
                         capture_output=True, text=True)
    assert run.returncode and "call to 'printf' has no definition in this program or the Squall " \
        "runtime library" in run.stderr, run.stderr


@requires_runtime
@pytest.mark.parametrize("cells,fits,aligned", [(30000, True, False), (30000, True, True), (70000, False, False)])
def test_large_programs_move_the_frame_arena(tmp_path, cells, fits, aligned):
    """Code and globals past a quarter of memory push the frame arena up,
    past any alignment padding; a program that leaves no room for frames is
    told the size it needs."""
    source = tmp_path / "large.c"
    attribute = "__attribute__((aligned(256))) " if aligned else ""
    source.write_text(f"""static int table[{cells}] = {{1, 2, 3}};
{attribute}int last(void) {{ return table[{cells} - 1]; }}
int main(int i) {{
  table[i] += 40;
  return table[i] + last();
}}
""")
    run = subprocess.run([clang(), "--target=squallv2", "-O2", str(source), "-o", str(tmp_path / "large.o")],
                         capture_output=True, text=True)
    if not fits:
        assert run.returncode and "leaving no room for frames in the 512 KiB image; use " \
            "-squall-memory-bytes=" in run.stderr, run.stderr
        return
    assert run.returncode == 0, run.stderr
    load(tmp_path / "large.o", tmp_path / "large", [2])
    manifest = json.loads((tmp_path / "large/manifest.json").read_text())
    assert manifest["root_frame_word"] > manifest["memory_bytes"] // 32  # Past the quarter mark.
    result = run_image(tmp_path / "large")
    assert result["status"] == 1 and result["result"] == 43, result


@pytest.mark.parametrize("level", ["-O1", "-O2"])
def test_aligned_loops_run(tmp_path, level):
    """-falign-loops pads before loop headers that code falls through into.
    It is a code generation option, so it applies with -c, not at the link."""
    source = tmp_path / "loops.c"
    source.write_text("""int data[16];
int main(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) data[i & 15] = i;
  for (int j = 0; j < n; j++) {
    int k = data[j & 15];
    while (k > 0) { s += k; k -= 3; }
  }
  return s;
}
""")
    if not clang():
        pytest.skip("the built Squall Clang is required")
    image = tmp_path / "loops.o"
    subprocess.run([clang(), "--target=squallv2", "-c", level, "-falign-loops=256", "-Wno-main",
                    str(source), "-o", str(image)], check=True, capture_output=True, text=True)
    load(image, tmp_path / "loops", [10])
    result = run_image(tmp_path / "loops")
    assert result["status"] == 1 and result["result"] == 72, result


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_aliased_main(tmp_path, level):
    """An alias of main names the entry function after aliases are resolved."""
    source = tmp_path / "alias.c"
    source.write_text('int real_main(void) { return 42; }\n'
                      'int main(void) __attribute__((alias("real_main")));\n')
    image = link(tmp_path, [source], "main", level)
    load(image, tmp_path / "alias")
    result = run_image(tmp_path / "alias")
    assert result["status"] == 1 and result["result"] == 42, result


@requires_runtime
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_used_main(tmp_path, level):
    """`used` puts main in llvm.compiler.used, where the runtime's unused
    functions are stripped; the entry must stay."""
    source = tmp_path / "used.c"
    source.write_text("__attribute__((used)) int main(void) { return 42; }\n")
    image = link(tmp_path, [source], "main", level)
    load(image, tmp_path / "used")
    result = run_image(tmp_path / "used")
    assert result["status"] == 1 and result["result"] == 42, result


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
    assert "'__mulsf3' has no definition in this program or the Squall runtime library, which " \
           "clang adds when it links (not with -c)" in run.stderr


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
