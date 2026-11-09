import json
"""LLVM IR compiled by llc, executed on the independent Zig interpreter."""
import operator

import pytest

from conftest import compile_ir, execute, i32, load, requires_llvm, run_tool
from squall_tools.reference import run_image

pytestmark = requires_llvm


def signed_div(a, b):
    return (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)


def expected_binary(op, a, b):
    ua, ub = a & 0xffffffff, b & 0xffffffff
    return i32({
        "add": lambda: a + b, "sub": lambda: a - b, "mul": lambda: a * b,
        "and": lambda: a & b, "or": lambda: a | b, "xor": lambda: a ^ b,
        "sdiv": lambda: signed_div(a, b), "udiv": lambda: ua // ub,
        "srem": lambda: a - signed_div(a, b) * b, "urem": lambda: ua % ub,
        "shl": lambda: a << b, "lshr": lambda: ua >> b, "ashr": lambda: a >> b,
    }[op]())


OPS = ["add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor", "shl", "lshr", "ashr"]
PAIRS = [(0, 1), (7, 3), (-7, 3), (7, -3), (-7, -3), (-2147483648, 31), (2147483647, 31), (-1, 1)]


@pytest.mark.parametrize("op", OPS)
def test_register_operations(tmp_path, op, opt):
    """One image per operation; every input pair is loaded into it separately."""
    ir = f"define i32 @main(i32 %a, i32 %b) {{ %r = {op} i32 %a, %b\nret i32 %r }}"
    obj_dir = compile_ir(tmp_path / "build", ir, [0, 1], opt=opt).parent
    for index, (a, b) in enumerate(PAIRS):
        if op in ("shl", "lshr", "ashr"):
            b &= 31
        load(obj_dir / "image.o", tmp_path / f"run{index}", [a, b])
        result = run_image(tmp_path / f"run{index}")
        assert result["status"] == 1 and result["result"] == expected_binary(op, a, b), (a, b, result)


@pytest.mark.parametrize("op,constant", [("add", -2147483648), ("mul", -7), ("sdiv", 3),
                                         ("udiv", 5), ("urem", 7), ("srem", -3), ("and", 255),
                                         ("or", -256), ("xor", 0x5a5a5a5a), ("shl", 31),
                                         ("lshr", 1), ("ashr", 31), ("udiv", -2)])
@pytest.mark.parametrize("a", [-2147483648, -5, 0, 9, 2147483647])
def test_immediate_operations(tmp_path, op, constant, a, opt):
    ir = f"define i32 @main(i32 %a) {{ %r = {op} i32 %a, {constant}\nret i32 %r }}"
    execute(tmp_path, ir, [a], expected_binary(op, a, constant), opt=opt)


@pytest.mark.parametrize("pred", ["eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"])
def test_comparisons(tmp_path, pred, opt):
    ir = (f"define i32 @main(i32 %a, i32 %b) {{ %c = icmp {pred} i32 %a, %b\n"
          "%r = zext i1 %c to i32\nret i32 %r }")
    build = compile_ir(tmp_path / "build", ir, [0, 0], opt=opt).parent
    for index, (a, b) in enumerate([(0, 0), (-1, 0), (0, -1), (-2147483648, 2147483647),
                                    (2147483647, -2147483648), (-7, -3), (-3, -7), (5, 5)]):
        left, right = (a & 0xffffffff, b & 0xffffffff) if pred.startswith("u") else (a, b)
        name = pred[1:] if pred[0] in "su" else pred
        expected = {"eq": operator.eq, "ne": operator.ne, "lt": operator.lt, "le": operator.le,
                    "gt": operator.gt, "ge": operator.ge}[name](left, right)
        load(build / "image.o", tmp_path / f"run{index}", [a, b])
        result = run_image(tmp_path / f"run{index}")
        assert result["status"] == 1 and result["result"] == int(expected), (a, b, result)


@pytest.mark.parametrize("a,b", [(0, 0), (0, 1), (1, 0), (1, 1)])
@pytest.mark.parametrize("pred", ["slt", "sge", "ult", "ugt", "eq", "ne"])
def test_boolean_values(tmp_path, a, b, pred):
    left, right = (-a, -b) if pred.startswith("s") else (a, b)
    name = pred[1:] if pred[0] in "su" else pred
    expected = {"lt": operator.lt, "gt": operator.gt, "ge": operator.ge,
                "eq": operator.eq, "ne": operator.ne}[name](left, right)
    execute(tmp_path, f"define i1 @main(i1 %a, i1 %b) {{ %r = icmp {pred} i1 %a, %b\nret i1 %r }}",
            [a, b], int(expected))


@pytest.mark.parametrize("value", [0, 1, -1, -129, 255, -2147483648, 2147483647])
def test_narrow_integers_and_select(tmp_path, value, opt):
    ir = """define i32 @main(i32 %a) {
      %bit = trunc i32 %a to i1
      %signed = sext i1 %bit to i32
      %byte = trunc i32 %a to i8
      %b2 = add i8 %byte, 1
      %wide = sext i8 %b2 to i32
      %half = trunc i32 %a to i16
      %h2 = zext i16 %half to i32
      %c = icmp slt i32 %a, 0
      %x = select i1 %c, i32 %signed, i32 %wide
      %r = add i32 %x, %h2
      ret i32 %r
    }"""
    byte = (value + 1) & 0xff
    wide = byte - 256 if byte >= 128 else byte
    x = -(value & 1) if value < 0 else wide
    execute(tmp_path, ir, [value], i32(x + (value & 0xffff)), opt=opt)


@pytest.mark.parametrize("condition", [0, 1])
def test_branch_phi(tmp_path, condition, opt):
    execute(tmp_path, """define i32 @main(i1 %c) {
    entry: br i1 %c, label %left, label %right
    left: br label %join
    right: br label %join
    join: %value = phi i32 [42, %left], [-7, %right]
      ret i32 %value
    }""", [condition], 42 if condition else -7, opt=opt)


@pytest.mark.parametrize("count", [0, 1, 2, 7, 20])
def test_loop_parallel_phi(tmp_path, count, opt):
    execute(tmp_path, """define i32 @main(i32 %n) {
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
    }""", [count], 18 if count & 1 else -18, opt=opt)


def test_long_loop_keeps_one_control_token(tmp_path, opt):
    result = execute(tmp_path, """define i32 @main() {
    entry: br label %loop
    loop: %i = phi i32 [0, %entry], [%next, %loop]
      %next = add i32 %i, 1
      %c = icmp slt i32 %next, 1000
      br i1 %c, label %loop, label %exit
    exit: ret i32 %next
    }""", expected=1000, opt=opt)
    assert result["peak_tokens"] <= 2


FIBONACCI = """define i32 @main(i32 %n) {
entry: %small = icmp slt i32 %n, 2
  br i1 %small, label %base, label %recurse
base: ret i32 %n
recurse:
  %n1 = sub i32 %n, 1
  %a = call i32 @main(i32 %n1)
  %n2 = sub i32 %n, 2
  %b = call i32 @main(i32 %n2)
  %r = add i32 %a, %b
  ret i32 %r
}"""


@pytest.mark.parametrize("n,expected", [(0, 0), (1, 1), (2, 1), (5, 5), (8, 21)])
def test_recursive_calls(tmp_path, n, expected, opt):
    execute(tmp_path, FIBONACCI, [n], expected, opt=opt)


DEPTH = """define i32 @main(i32 %n) {
  %z = icmp eq i32 %n, 0
  br i1 %z, label %done, label %more
done: ret i32 0
more: %m = sub i32 %n, 1
  %r = call i32 @main(i32 %m)
  %s = add i32 %r, 1
  ret i32 %s
}"""


def test_call_depth_limit(tmp_path):
    small = ["-squall-memory-bytes=65536"]
    execute(tmp_path / "ok", DEPTH, [100], 100, options=small)
    execute(tmp_path / "deep", DEPTH, [100000], status=2, options=small)


def test_caller_values_survive_calls(tmp_path, opt):
    execute(tmp_path, """define i32 @main(i32 %a, i32 %b, i1 %c) {
      %saved = mul i32 %a, 3
      %one = call i32 @choose(i1 %c, i32 %a, i32 %b)
      %two = call i32 @choose(i1 false, i32 %b, i32 %a)
      %both = add i32 %one, %two
      %r = sub i32 %both, %saved
      ret i32 %r
    }
    define i32 @choose(i1 %condition, i32 %x, i32 %y) noinline {
      %r = select i1 %condition, i32 %x, i32 %y
      ret i32 %r
    }""", [7, 29, 1], -7, opt=opt)


def test_sixteen_call_arguments(tmp_path, opt):
    params = ", ".join(f"i32 %p{i}" for i in range(16))
    body = "\n".join(f"  %m{i} = mul i32 %p{i}, {i + 1}\n  %s{i + 1} = add i32 %s{i}, %m{i}" for i in range(16))
    args = ", ".join(f"i32 %a{i % 4}" for i in range(16))
    ir = f"""define i32 @sum({params}) noinline {{
  %s0 = add i32 0, 0
{body}
  ret i32 %s16
}}
define i32 @main(i32 %a0, i32 %a1, i32 %a2, i32 %a3) {{
  %r = call i32 @sum({args})
  ret i32 %r
}}"""
    values = [3, -4, 10, 7]
    expected = sum(values[i % 4] * (i + 1) for i in range(16))
    execute(tmp_path, ir, values, expected, opt=opt)


def test_globals_locals_and_void_calls(tmp_path, opt):
    execute(tmp_path, """@g = global i32 7
    define void @set(i32 %a) noinline { store i32 %a, ptr @g
      ret void }
    define i32 @main() {
      %p = alloca i32
      store i32 11, ptr %p
      %a = load i32, ptr @g
      call void @set(i32 29)
      %b = load i32, ptr @g
      %c = load i32, ptr %p
      %ab = add i32 %a, %b
      %r = add i32 %ab, %c
      ret i32 %r
    }""", expected=47, opt=opt)


@pytest.mark.parametrize("index,expected", [(0, 7), (1, 8), (2, -2), (3, 16)])
def test_array_index_load_store(tmp_path, index, expected, opt):
    execute(tmp_path, """@a = global [4 x i32] [i32 2, i32 3, i32 -7, i32 11]
    define i32 @main(i32 %i) {
      %p = getelementptr [4 x i32], ptr @a, i32 0, i32 %i
      %v = load i32, ptr %p, align 4
      %r = add i32 %v, 5
      store i32 %r, ptr %p, align 4
      %w = load i32, ptr %p, align 4
      ret i32 %w
    }""", [index], expected, opt=opt)


def test_pointer_calls_returns_alias_and_negative_gep(tmp_path, opt):
    execute(tmp_path, """define ptr @previous(ptr %p) noinline {
      %q = getelementptr i32, ptr %p, i32 -1
      ret ptr %q
    }
    define void @update(ptr %p) noinline { store i32 -91, ptr %p, align 4
      ret void }
    define i32 @main() {
      %array = alloca i32, i32 4
      %end = getelementptr i32, ptr %array, i32 4
      %last = call ptr @previous(ptr %end)
      %holder = alloca ptr
      store ptr %last, ptr %holder, align 4
      %alias = load ptr, ptr %holder, align 4
      call void @update(ptr %alias)
      %result = load i32, ptr %last, align 4
      ret i32 %result
    }""", expected=-91, opt=opt)


@pytest.mark.parametrize("choice,expected", [(0, 17), (1, 31)])
def test_pointer_phi_and_select(tmp_path, choice, expected, opt):
    execute(tmp_path, """@a = global i32 17
    @b = global i32 31
    define i32 @main(i1 %c) {
      br i1 %c, label %yes, label %no
    yes: br label %end
    no: br label %end
    end:
      %p = phi ptr [@b, %yes], [@a, %no]
      %q = select i1 %c, ptr %p, ptr @a
      %v = load i32, ptr %q, align 4
      ret i32 %v
    }""", [choice], expected, opt=opt)


def test_structs_and_global_pointer_initializers(tmp_path, opt):
    execute(tmp_path, """%pair = type { i32, ptr }
    @x = global i32 40
    @p = global %pair { i32 2, ptr @x }
    define i32 @main() {
      %f = getelementptr %pair, ptr @p, i32 0, i32 1
      %ptr = load ptr, ptr %f
      %v = load i32, ptr %ptr
      %k = load i32, ptr @p
      %local = alloca %pair
      %g = getelementptr %pair, ptr %local, i32 0, i32 0
      store i32 %k, ptr %g
      %w = load i32, ptr %g
      %r = add i32 %v, %w
      ret i32 %r
    }""", expected=42, opt=opt)


def test_globals_beyond_absolute_slot_range(tmp_path, opt):
    # Globals past word 2047 cannot use 11-bit absolute slots; accesses use
    # the dynamic memory helper with a constant pointer.
    execute(tmp_path, """@big = global [3000 x i32] zeroinitializer
    @after = global i32 5
    define i32 @main(i32 %i) {
      %p = getelementptr [3000 x i32], ptr @big, i32 0, i32 %i
      store i32 37, ptr %p
      %v = load i32, ptr %p
      %old = load i32, ptr @after
      %sum = add i32 %old, %v
      store i32 %sum, ptr @after
      %r = load i32, ptr @after
      ret i32 %r
    }""", [2999], 42, opt=opt)


def test_global_aliases(tmp_path, opt):
    # An alias names its aliasee, possibly at an offset; the image has no
    # symbols, so every use resolves to the variable itself. Without LTO's
    # optimizations (clang -O0) aliases reach code generation intact.
    execute(tmp_path, """@x = global [2 x i32] [i32 5, i32 9]
    @y = alias i32, getelementptr (i8, ptr @x, i32 4)
    @z = alias i32, ptr @x
    define ptr @addr() noinline { ret ptr @y }
    define i32 @main() {
      %p = call ptr @addr()
      %v = load i32, ptr %p
      store i32 11, ptr @z
      %a = load i32, ptr @x
      %same = icmp eq ptr %p, getelementptr (i8, ptr @x, i32 4)
      %s = zext i1 %same to i32
      %h = mul i32 %v, 100
      %t = mul i32 %s, 1000
      %r1 = add i32 %h, %a
      %r = add i32 %r1, %t
      ret i32 %r
    }""", expected=1911, opt=opt)


def test_preferred_alignment_padding_moves_the_frame_arena(tmp_path, opt):
    # .prefalign pads before a function up to its preferred alignment; with
    # code past a quarter of memory, the frame arena must start past that
    # padding as well.
    execute(tmp_path, """@table = internal global [30000 x i32] zeroinitializer
    define internal i32 @helper(i32 %x) noinline {
      %y = add i32 %x, 1
      ret i32 %y
    }
    define i32 @main(i32 %i) prefalign(256) {
      %p = getelementptr [30000 x i32], ptr @table, i32 0, i32 %i
      store i32 41, ptr %p
      %v = load i32, ptr %p
      %w = call i32 @helper(i32 %v)
      ret i32 %w
    }""", [29999], 42, opt=opt)
    manifest = json.loads((tmp_path / "image/manifest.json").read_text())
    assert manifest["symbols"]["main"] % 32 == 0, manifest["symbols"]  # 256-byte aligned.


def test_fall_through_into_aligned_blocks(tmp_path):
    # Every block aligned to 256 bytes: code that falls into the next block
    # must jump over the alignment padding, which is not executable.
    # (Block placement, which applies the alignment, does not run at -O0.)
    execute(tmp_path, """define i32 @main(i32 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %s = phi i32 [ 0, %entry ], [ %sum, %loop ]
      %t = mul i32 %i, 3
      %sum = add i32 %s, %t
      %next = add i32 %i, 1
      %more = icmp slt i32 %next, %n
      br i1 %more, label %loop, label %done
    done:
      %r = add i32 %sum, 1
      ret i32 %r
    }""", [10], 136, opt="-O2", options=("-align-all-blocks=8",))


def test_aliased_entry_function(tmp_path, opt):
    # The entry is found by name; an alias that names it must still start the
    # program after aliases are resolved, even when the aliasee is internal.
    execute(tmp_path, """define internal i32 @real(i32 %x) {
      %r = add i32 %x, 40
      ret i32 %r
    }
    @main = alias i32 (i32), ptr @real""", [2], 42, opt=opt)


@pytest.mark.parametrize("order", ["entry-first", "entry-last"])
def test_chained_entry_aliases(tmp_path, order):
    # The entry resolves through a chain of aliases whatever their order.
    aliases = ["@main = alias i32 (), ptr @mid", "@mid = alias i32 (), ptr @real"]
    if order == "entry-last":
        aliases.reverse()
    execute(tmp_path, "define internal i32 @real() {\n  ret i32 42\n}\n" + "\n".join(aliases),
            expected=42)


def test_entry_marked_used(tmp_path, opt):
    # __attribute__((used)) puts main in llvm.compiler.used, beside the
    # runtime functions that are stripped when nothing calls them.
    execute(tmp_path, """@llvm.compiler.used = appending global [1 x ptr] [ptr @main], section "llvm.metadata"
    define i32 @main() {
      ret i32 42
    }""", expected=42, opt=opt)


@pytest.mark.parametrize("kind,value,expected", [
    ("float", 0x7f800001, 0x7fc00001),   # Signaling NaN: quieted, payload kept.
    ("float", 0xff800001, 0xffc00001),   # Negative signaling NaN.
    ("float", 0x7fc00005, 0x7fc00005),   # Quiet NaN.
    ("float", 0x7f800000, 0x7f800000),   # Infinity.
    ("float", 0x00000001, 0x00000001),   # Subnormal: kept in IEEE mode.
    ("float", 0x80000000, 0x80000000),   # -0.0.
    ("double", 0x7ff00000, 0x7ff80000),  # High word of a signaling NaN.
    ("double", 0x7ff80000, 0x7ff80000),  # Quiet NaN.
    ("double", 0x3ff00000, 0x3ff00000),  # 1.0.
])
def test_canonicalize_quiets_signaling_nans(tmp_path, kind, value, expected, opt):
    if kind == "float":
        ir = """define i32 @main(i32 %bits) {
          %f = bitcast i32 %bits to float
          %c = call float @llvm.canonicalize.f32(float %f)
          %r = bitcast float %c to i32
          ret i32 %r
        }
        declare float @llvm.canonicalize.f32(float)"""
        arguments = [value]
    else:
        # The low word 1 makes 0x7ff00000 a signaling NaN rather than infinity.
        ir = """define i32 @main(i32 %lo, i32 %hi) {
          %l = zext i32 %lo to i64
          %h0 = zext i32 %hi to i64
          %h = shl i64 %h0, 32
          %bits = or i64 %h, %l
          %d = bitcast i64 %bits to double
          %c = call double @llvm.canonicalize.f64(double %d)
          %cb = bitcast double %c to i64
          %top = lshr i64 %cb, 32
          %r = trunc i64 %top to i32
          ret i32 %r
        }
        declare double @llvm.canonicalize.f64(double)"""
        arguments = [1, value]
    execute(tmp_path, ir, arguments, i32(expected), opt=opt)


def test_high_bit_pointer_arguments(tmp_path):
    # Registers hold pointers zero-extended like integers, so a loaded
    # pointer equals the same (negative-literal) constant in code.
    execute(tmp_path, """define i32 @main(ptr %p) {
      %eq = icmp eq ptr %p, inttoptr (i32 -2147483648 to ptr)
      %r = zext i1 %eq to i32
      ret i32 %r
    }""", [0x80000000], 1)


def test_null_load_traps_before_access(tmp_path):
    execute(tmp_path, "define i32 @main(ptr %p) { %v = load i32, ptr %p, align 4\n ret i32 %v }",
            [0], status=3)


@pytest.mark.parametrize("offset", [-16, 65536, 536870911])
def test_memory_range_guard(tmp_path, offset):
    execute(tmp_path, f"""@value = global i32 42
      define i32 @main(i32 %i) {{
        %p = getelementptr i32, ptr @value, i32 %i
        %result = load i32, ptr %p, align 4
        ret i32 %result
      }}""", [offset], status=3)


# A memory lane beside an arithmetic lane (-squall-fork-join). A guard that
# fails in one lane halts the program; the other lane stops at the join.
LANE_GUARD = """@value = global [4 x i32] [i32 11, i32 22, i32 33, i32 44]
define i32 @main(i32 %i, i32 %b) {
  %p = getelementptr i32, ptr @value, i32 %i
  %v = load i32, ptr %p, align 4
  %v2 = mul i32 %v, 3
  %v3 = xor i32 %v2, 5
  %b1 = mul i32 %b, 22695477
  %b2 = lshr i32 %b1, 7
  %b3 = xor i32 %b2, %b1
  %r = add i32 %v3, %b3
  ret i32 %r
}"""


def lane_guard_expected(i, b):
    v = [11, 22, 33, 44][i] * 3 ^ 5
    b1 = (b * 22695477) & 0xFFFFFFFF
    return i32(v + ((b1 >> 7) ^ b1))


@pytest.mark.parametrize("i,status", [(1, 1), (3, 1), (-16, 3), (65536, 3), (536870911, 3)])
def test_memory_guard_inside_parallel_lane(tmp_path, opt, i, status):
    result = execute(tmp_path, LANE_GUARD, [i, 77], lane_guard_expected(i, 77) if status == 1 else None,
                     status=status, opt=opt, options=["-squall-fork-join"])
    manifest = json.loads((tmp_path / "image/manifest.json").read_text())
    assert manifest["parallel_lanes"], "the load and the arithmetic should form two lanes"


def test_memset_and_memcpy(tmp_path, opt):
    execute(tmp_path, """declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)
    declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
    define i32 @main() {
      %a = alloca [4 x i32]
      %b = alloca [4 x i32]
      call void @llvm.memset.p0.i32(ptr align 4 %a, i8 -1, i32 16, i1 false)
      call void @llvm.memcpy.p0.p0.i32(ptr align 4 %b, ptr align 4 %a, i32 16, i1 false)
      %last = getelementptr [4 x i32], ptr %b, i32 0, i32 3
      %v = load i32, ptr %last, align 4
      ret i32 %v
    }""", expected=-1, opt=opt)


def test_overlapping_memmove(tmp_path, opt):
    execute(tmp_path, """@a = global [8 x i32] [i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8]
    declare void @llvm.memmove.p0.p0.i32(ptr, ptr, i32, i1)
    define i32 @main() {
      %d = getelementptr i32, ptr @a, i32 1
      call void @llvm.memmove.p0.p0.i32(ptr align 4 %d, ptr align 4 @a, i32 16, i1 false)
      %p = getelementptr i32, ptr @a, i32 4
      %v = load i32, ptr %p
      %q = getelementptr i32, ptr @a, i32 1
      %w = load i32, ptr %q
      %r = mul i32 %v, 10
      %s = add i32 %r, %w
      ret i32 %s
    }""", expected=41, opt=opt)


def test_frame_objects_keep_alignment(tmp_path):
    image = compile_ir(tmp_path, """define i32 @main() {
      %a = alloca i32
      %b = alloca [4 x i32], align 16
      %addr = ptrtoint ptr %b to i32
      %r = and i32 %addr, 15
      ret i32 %r
    }""")
    assert run_image(image)["result"] == 0


@pytest.mark.parametrize("program", ["add", "fibonacci", "memory"])
def test_assembly_round_trips_to_identical_image(tmp_path, program):
    ir = {"add": "define i32 @main(i32 %a, i32 %b) { %r = add i32 %a, %b\nret i32 %r }",
          "fibonacci": FIBONACCI,
          "memory": """@a = global [4 x i32] zeroinitializer
            define i32 @main(i32 %i) { %p = getelementptr [4 x i32], ptr @a, i32 0, i32 %i
            store i32 3, ptr %p
            %v = load i32, ptr %p
            ret i32 %v }"""}[program]
    source = tmp_path / "input.ll"
    source.write_text(ir)
    run_tool("llc", "-mtriple=squallv2", "-filetype=obj", source, "-o", tmp_path / "direct.o")
    run_tool("llc", "-mtriple=squallv2", source, "-o", tmp_path / "text.s")
    run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", tmp_path / "text.s", "-o", tmp_path / "text.o")
    sections = []
    for name in ("direct", "text"):
        out = tmp_path / f"{name}.bin"
        run_tool("llvm-objcopy", f"--dump-section=.squall.image={out}", tmp_path / f"{name}.o",
                 tmp_path / f"{name}-copy.o")
        sections.append(out.read_bytes())
    assert sections[0] == sections[1]
    assert not run_tool("llvm-readobj", "-r", tmp_path / "direct.o").stdout.count("R_SQUALL")


WIDE = """define i64 @op(i64 %a, i64 %b) noinline {
  %r = OP i64 %a, %b
  ret i64 %r
}
define i32 @main(i32 %alo, i32 %ahi, i32 %blo, i32 %bhi) {
  %al = zext i32 %alo to i64
  %ah = zext i32 %ahi to i64
  %bl = zext i32 %blo to i64
  %bh = zext i32 %bhi to i64
  %ahs = shl i64 %ah, 32
  %bhs = shl i64 %bh, 32
  %a = or i64 %ahs, %al
  %b = or i64 %bhs, %bl
  %r = call i64 @op(i64 %a, i64 %b)
  %lo = trunc i64 %r to i32
  %hs = lshr i64 %r, 32
  %hi = trunc i64 %hs to i32
  %h3 = mul i32 %hi, 3
  %sum = add i32 %lo, %h3
  ret i32 %sum
}"""


# Division is expanded into IR loops: there is no runtime library.
@pytest.mark.parametrize("op", ["add", "sub", "mul", "and", "xor", "shl", "lshr", "ashr",
                                "sdiv", "udiv", "srem", "urem"])
def test_64_bit_integers(tmp_path, op, opt):
    build = compile_ir(tmp_path / "build", WIDE.replace("OP", op), [0, 0, 0, 0], opt=opt).parent
    mask = (1 << 64) - 1
    for index, (a, b) in enumerate([(3, 5), (mask, 2), (1 << 63, (1 << 32) + 7),
                                    (0x123456789abcdef0, 0x0fedcba987654321)]):
        if op in ("shl", "lshr", "ashr"):
            b %= 64
        signed = a - (1 << 64) if a >> 63 else a
        signed_b = b - (1 << 64) if b >> 63 else b
        r = {"add": lambda: a + b, "sub": lambda: a - b, "mul": lambda: a * b,
             "and": lambda: a & b, "xor": lambda: a ^ b, "shl": lambda: a << b,
             "lshr": lambda: a >> b, "ashr": lambda: signed >> b,
             "sdiv": lambda: signed_div(signed, signed_b), "udiv": lambda: a // b,
             "srem": lambda: signed - signed_div(signed, signed_b) * signed_b,
             "urem": lambda: a % b}[op]() & mask
        expected = i32((r & 0xffffffff) + 3 * (r >> 32))
        load(build / "image.o", tmp_path / f"run{index}",
             [i32(a), i32(a >> 32), i32(b), i32(b >> 32)])
        result = run_image(tmp_path / f"run{index}")
        assert result["status"] == 1 and result["result"] == expected, (a, b, result)


# Byte memory: a value of each width is stored at each byte offset of a
# patterned buffer, then read back; a checksum of every cell shows that the
# neighboring bytes are intact. "dynamic" passes the address through a
# noinline call so the byte position is computed at run time.
NARROW = """@g = global [16 x i8] zeroinitializer
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)
define void @put(ptr %p, TYPE %v) noinline {
  store TYPE %v, ptr %p, align ALIGN
  ret void
}
define i32 @get(ptr %p) noinline {
  %v = load TYPE, ptr %p, align ALIGN
  %r = EXTEND
  ret i32 %r
}
define i32 @main(i32 %value, i32 %offset) {
  call void @llvm.memset.p0.i32(ptr align 4 @g, i8 -91, i32 16, i1 false)
  %v = TRUNC
  ACCESS
  %c0 = load i32, ptr @g
  %c1 = load i32, ptr getelementptr (i8, ptr @g, i32 4)
  %c2 = load i32, ptr getelementptr (i8, ptr @g, i32 8)
  %c3 = load i32, ptr getelementptr (i8, ptr @g, i32 12)
  %s1 = mul i32 %c0, 31
  %t1 = add i32 %s1, %c1
  %s2 = mul i32 %t1, 31
  %t2 = add i32 %s2, %c2
  %s3 = mul i32 %t2, 31
  %t3 = add i32 %s3, %c3
  %x = xor i32 %r, %t3
  ret i32 %x
}"""

NARROW_CASES = [(bits, offset, align, extend)
                for bits in (8, 16, 32)
                for offset in range(8)
                for align in sorted({1, bits // 8})
                if offset % align == 0
                for extend in (("sext", "zext") if bits < 32 else ("none",))]


@pytest.mark.parametrize("dynamic", [False, True], ids=["static", "dynamic"])
@pytest.mark.parametrize("bits,offset,align,extend", NARROW_CASES)
def test_narrow_and_misaligned_memory(tmp_path, bits, offset, align, extend, dynamic, opt):
    t = f"i{bits}"
    access = ("%p = getelementptr i8, ptr @g, i32 %offset\n"
              "  call void @put(ptr %p, TYPE %v)\n  %r = call i32 @get(ptr %p)") if dynamic else (
              f"%p = getelementptr i8, ptr @g, i32 {offset}\n"
              "  store TYPE %v, ptr %p, align ALIGN\n  %w = load TYPE, ptr %p, align ALIGN\n"
              "  %r = EXTEND_W")
    ir = (NARROW.replace("ACCESS", access)
          .replace("EXTEND_W", f"{extend} {t} %w to i32" if bits < 32 else "add i32 %w, 0")
          .replace("EXTEND", f"{extend} {t} %v to i32" if bits < 32 else "add i32 %v, 0")
          .replace("TRUNC", f"trunc i32 %value to {t}" if bits < 32 else "add i32 %value, 0")
          .replace("TYPE", t).replace("ALIGN", str(align)))
    build = compile_ir(tmp_path / "build", ir, [0, 0], opt=opt).parent
    size = bits // 8
    for index, value in enumerate([0x1234abcd, -1, 0x80, -0x7f7f7f80]):
        memory = bytearray([0xa5] * 16)
        memory[offset:offset + size] = (value & ((1 << bits) - 1)).to_bytes(size, "little")
        loaded = int.from_bytes(memory[offset:offset + size], "little", signed=extend != "zext")
        checksum = 0
        for cell in range(4):
            checksum = checksum * 31 + int.from_bytes(memory[4 * cell:4 * cell + 4], "little")
        load(build / "image.o", tmp_path / f"run{index}", [i32(value), offset])
        result = run_image(tmp_path / f"run{index}")
        assert result["status"] == 1 and result["result"] == i32(loaded ^ checksum), (value, result)


def test_byte_initializers_and_interior_pointers(tmp_path, opt):
    execute(tmp_path, """@s = global [6 x i8] c"hello\\00"
    @p = global ptr getelementptr (i8, ptr @s, i32 2)
    @mix = global { i8, i16, i8, i32 } { i8 -1, i16 -300, i8 7, i32 123456 }
    @packed = global <{ i8, i32, i16 }> <{ i8 3, i32 -559038737, i16 4660 }>
    define i32 @main() {
      %q = load ptr, ptr @p
      %c = load i8, ptr %q
      %c32 = zext i8 %c to i32
      %a = load i8, ptr @mix
      %a32 = sext i8 %a to i32
      %b = load i16, ptr getelementptr (i8, ptr @mix, i32 2)
      %b32 = sext i16 %b to i32
      %d = load i8, ptr getelementptr (i8, ptr @mix, i32 4)
      %d32 = zext i8 %d to i32
      %e = load i32, ptr getelementptr (i8, ptr @mix, i32 8)
      %f = load i32, ptr getelementptr (i8, ptr @packed, i32 1), align 1
      %h = load i16, ptr getelementptr (i8, ptr @packed, i32 5), align 1
      %h32 = zext i16 %h to i32
      %s1 = add i32 %c32, %a32
      %s2 = add i32 %s1, %b32
      %s3 = mul i32 %d32, 1000
      %s4 = add i32 %s2, %s3
      %s5 = add i32 %s4, %e
      %s6 = xor i32 %s5, %f
      %s7 = add i32 %s6, %h32
      ret i32 %s7
    }""", expected=i32((((108 - 1 - 300 + 7000 + 123456) ^ -559038737) + 4660)), opt=opt)


# Floating-point stores below cell alignment are split into bytes, keeping
# their bit pattern; zero-extending them, as for integers, is invalid IR.
PACKED_FLOAT = """@g = global [16 x i8] zeroinitializer, align 4
define i32 @main(i32 %bits, i32 %offset) {
  %p = getelementptr [16 x i8], ptr @g, i32 0, i32 %offset
  %f = bitcast i32 %bits to float
  store volatile float %f, ptr %p, align 1
  %wide = zext i32 %bits to i64
  %high = shl i64 %wide, 32
  %d = bitcast i64 %high to double
  %q = getelementptr i8, ptr %p, i32 4
  store volatile double %d, ptr %q, align 1
  %v = load volatile float, ptr %p, align 1
  %vb = bitcast float %v to i32
  %hi = getelementptr i8, ptr %q, i32 4
  %h = load volatile i32, ptr %hi, align 1
  %r = xor i32 %vb, %h
  %s = xor i32 %r, 1
  ret i32 %s
}"""


@pytest.mark.parametrize("offset", [1, 2, 3])
def test_packed_floating_point_stores(tmp_path, opt, offset):
    # The float reads back as its bits; the double's high word is the same
    # bits, so their xor leaves only the final 1.
    execute(tmp_path, PACKED_FLOAT, [0x3FC00000, offset], 1, opt=opt)


# The alloca area is exhausted, not wrapped, by counts whose byte size
# overflows 32 bits, before or after rounding to whole cells.
@pytest.mark.parametrize("element,count,status", [
    ("i32", 1023, 1), ("i32", 1024, 1), ("i32", 1025, 4), ("i32", 1073741824, 4),
    ("i8", 4096, 1), ("i8", 4097, 4), ("i8", 4294967295, 4), ("i64", 536870912, 4),
])
def test_alloca_size_overflow_traps(tmp_path, opt, element, count, status):
    to_i32 = {"i8": "zext i8 %v to i32", "i32": "add i32 %v, 0", "i64": "trunc i64 %v to i32"}
    execute(tmp_path, f"""define i32 @main(i32 %n) {{
      %a = alloca {element}, i32 %n, align 4
      store volatile {element} 42, ptr %a, align 4
      %v = load volatile {element}, ptr %a, align 4
      %r = {to_i32[element]}
      ret i32 %r
    }}""", [count], 42 if status == 1 else None, status=status, opt=opt)
