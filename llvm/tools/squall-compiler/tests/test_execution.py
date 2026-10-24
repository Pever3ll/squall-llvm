"""Execute emitted machine words with the pinned, independent Zig interpreter."""
import operator

import pytest

from squall_compiler.compiler import compile_ir
from squall_compiler.reference import run_image


def execute(tmp_path, ir, arguments=(), expected=0, **options):
    image = compile_ir(ir, arguments=arguments, **options)
    image.write(tmp_path)
    result = run_image(tmp_path)
    assert result["quiescent"], result
    assert result["status"] == 1, result
    assert result["result"] == expected, result
    return image


def i32(value):
    value &= 0xffffffff
    return value - (1 << 32) if value >= 1 << 31 else value


def signed_div(a, b):
    return (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)


@pytest.mark.parametrize("a,b", [(0, 1), (7, 3), (-7, 3), (7, -3), (-7, -3),
                                  (-2147483648, 31), (2147483647, 31), (-1, 1)])
@pytest.mark.parametrize("op", ["add", "sub", "mul", "sdiv", "udiv", "srem", "urem",
                                 "and", "or", "xor", "shl", "lshr", "ashr"])
def test_integer_operations(tmp_path, op, a, b):
    if op in ("shl", "lshr", "ashr"):
        b &= 31
    unsigned_a, unsigned_b = a & 0xffffffff, b & 0xffffffff
    if op in ("add", "sub", "mul", "and", "or", "xor"):
        action = {"add": operator.add, "sub": operator.sub, "mul": operator.mul,
                  "and": operator.and_, "or": operator.or_, "xor": operator.xor}[op]
        expected = action(a, b)
    elif op == "sdiv":
        expected = signed_div(a, b)
    elif op == "udiv":
        expected = unsigned_a // unsigned_b
    elif op == "srem":
        expected = a - signed_div(a, b) * b
    elif op == "urem":
        expected = unsigned_a % unsigned_b
    else:
        expected = {"shl": lambda: a << b, "lshr": lambda: unsigned_a >> b,
                    "ashr": lambda: a >> b}[op]()
    execute(tmp_path, f"define i32 @main(i32 %a, i32 %b) {{ %r = {op} i32 %a, %b\nret i32 %r }}",
            [a, b], i32(expected))


@pytest.mark.parametrize("a,b", [(0, 0), (-1, 0), (0, -1), (-2147483648, 2147483647),
                                  (2147483647, -2147483648), (-7, -3), (-3, -7)])
@pytest.mark.parametrize("pred", ["eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"])
def test_comparisons(tmp_path, pred, a, b):
    left, right = (a & 0xffffffff, b & 0xffffffff) if pred.startswith("u") else (a, b)
    op = pred[1:] if pred[0] in "su" else pred
    expected = {"eq": operator.eq, "ne": operator.ne, "lt": operator.lt,
                "le": operator.le, "gt": operator.gt, "ge": operator.ge}[op](left, right)
    execute(tmp_path, f"define i32 @main(i32 %a, i32 %b) {{ %c = icmp {pred} i32 %a, %b\n"
            "%r = zext i1 %c to i32\nret i32 %r }", [a, b], int(expected))


@pytest.mark.parametrize("value", [0, 1, -1, -2147483648, 2147483647])
def test_fanout_select_cast(tmp_path, value):
    ir = """define i32 @main(i32 %a) {
      %bit = trunc i32 %a to i1
      %signed = sext i1 %bit to i32
      %c = icmp slt i32 %a, 0
      %twice = add i32 %a, %a
      %s = select i1 %c, i32 %signed, i32 %twice
      ret i32 %s
    }"""
    execute(tmp_path, ir, [value], -(value & 1) if value < 0 else i32(value * 2))


@pytest.mark.parametrize("condition", [0, 1])
def test_branch_phi(tmp_path, condition):
    execute(tmp_path, """define i32 @main(i1 %c) {
    entry: br i1 %c, label %left, label %right
    left: br label %join
    right: br label %join
    join: %value = phi i32 [42, %left], [-7, %right]
      ret i32 %value
    }""", [condition], 42 if condition else -7)


@pytest.mark.parametrize("count", [0, 1, 2, 7, 20])
def test_loop_parallel_phi(tmp_path, count):
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
    }""", [count], 18 if count & 1 else -18)


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


@pytest.mark.parametrize("n,expected", list(enumerate([0, 1, 1, 2, 3, 5, 8, 13, 21])))
def test_recursive_calls(tmp_path, n, expected):
    execute(tmp_path, FIBONACCI, [n], expected)


def test_memory_and_void_calls(tmp_path):
    execute(tmp_path, """@g = global i32 7
    define void @set(i32 %a) { store i32 %a, ptr @g
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
    }""", expected=47)


@pytest.mark.parametrize("frames", [1, 2, 3])
def test_recursion_limit(tmp_path, frames):
    image = compile_ir(FIBONACCI, arguments=[8], frames=frames)
    image.write(tmp_path)
    result = run_image(tmp_path)
    assert result["quiescent"]
    assert result["status"] == 2


def test_many_entry_arguments(tmp_path):
    execute(tmp_path, """define i32 @main(i32 %a, i32 %b, i32 %c, i32 %d) {
      %ab = add i32 %a, %b
      %cd = sub i32 %c, %d
      %r = mul i32 %ab, %cd
      ret i32 %r
    }""", [3, 4, 10, 4], 42)


def test_long_loop_uses_bounded_token_stack(tmp_path):
    execute(tmp_path, """define i32 @main() {
    entry: br label %loop
    loop: %i = phi i32 [0, %entry], [%next, %loop]
      %next = add i32 %i, 1
      %c = icmp slt i32 %next, 1000
      br i1 %c, label %loop, label %exit
    exit: ret i32 %next
    }""", expected=1000, stack_tokens=2)


@pytest.mark.parametrize("a,b", [(0, 0), (0, 1), (1, 0), (1, 1)])
@pytest.mark.parametrize("predicate", ["slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge", "eq", "ne"])
def test_i1_comparisons(tmp_path, a, b, predicate):
    left, right = (-a, -b) if predicate.startswith("s") else (a, b)
    op = predicate[1:] if predicate[0] in "su" else predicate
    expected = {"lt": operator.lt, "le": operator.le, "gt": operator.gt,
                "ge": operator.ge, "eq": operator.eq, "ne": operator.ne}[op](left, right)
    execute(tmp_path, f"define i1 @main(i1 %a, i1 %b) {{ %r = icmp {predicate} i1 %a, %b\nret i1 %r }}",
            [a, b], int(expected), stack_tokens=2)


def test_multiple_call_arguments_and_caller_values(tmp_path):
    execute(tmp_path, """define i32 @main(i32 %a, i32 %b, i1 %c) {
      %saved = mul i32 %a, 3
      %one = call i32 @choose(i1 %c, i32 %a, i32 %b)
      %two = call i32 @choose(i1 false, i32 %b, i32 %a)
      %both = add i32 %one, %two
      %r = sub i32 %both, %saved
      ret i32 %r
    }
    define i32 @choose(i1 %condition, i32 %x, i32 %y) {
      %r = select i1 %condition, i32 %x, i32 %y
      ret i32 %r
    }""", [7, 29, 1], -7, stack_tokens=3, frames=2)


def test_exact_frame_boundary(tmp_path):
    execute(tmp_path, FIBONACCI, [8], 21, frames=8, stack_tokens=2)
    compile_ir(FIBONACCI, arguments=[8], frames=7, stack_tokens=2).write(tmp_path)
    assert run_image(tmp_path)["status"] == 2
