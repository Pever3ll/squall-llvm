import os

import pytest

from squall_compiler.compiler import compile_ir
from squall_compiler.assembler import slot_word
from squall_compiler.rtl import run_rtl

from test_execution import FIBONACCI


pytestmark = pytest.mark.skipif(not os.environ.get("SQUALL_ISACOMP_DIR"),
                                reason="set SQUALL_ISACOMP_DIR to run pinned RTL integration tests")


@pytest.mark.parametrize("n,expected", [(0, 0), (5, 5), (8, 21)])
def test_compiled_recursion_on_rtl(tmp_path, n, expected):
    compile_ir(FIBONACCI, arguments=[n], stack_tokens=2).write(tmp_path)
    result = run_rtl(tmp_path)
    assert result["result"] == expected
    assert result["trace_matches"]


def test_compiled_memory_and_branches_on_rtl(tmp_path):
    compile_ir("""@g = global i32 3
    define i32 @main() {
    entry: %local = alloca i32
      store i32 4, ptr %local
      br label %loop
    loop: %i = phi i32 [0, %entry], [%next, %loop]
      %a = load i32, ptr @g
      %b = load i32, ptr %local
      %sum = add i32 %a, %b
      store i32 %sum, ptr @g
      %next = add i32 %i, 1
      %more = icmp slt i32 %next, 10
      br i1 %more, label %loop, label %done
    done: %r = load i32, ptr @g
      ret i32 %r
    }""", stack_tokens=2).write(tmp_path)
    assert run_rtl(tmp_path)["result"] == 43


def test_runner_detects_memory_corruption_even_when_result_matches(tmp_path):
    image = compile_ir("define i32 @main() { ret i32 42 }")
    image.write(tmp_path)
    unused = image.manifest["root_frame_word"] + (image.manifest["max_frames"] - 1) * image.manifest["frame_stride_words"] + 1
    original = (tmp_path / "memory.hex").read_text()
    (tmp_path / "memory.hex").write_text(original.replace(
        f"@{unused:x} {slot_word(0):016x}", f"@{unused:x} {slot_word(1):016x}"))
    with pytest.raises(RuntimeError, match="RTL/reference mismatch"):
        run_rtl(tmp_path)
    import json
    result = json.loads((tmp_path / "rtl-result.json").read_text())
    assert result["result"] == 42 and result["status"] == 1 and result["trace_matches"]
    assert not result["memory_matches"]
    assert result["memory_differences"][0]["word"] == unused


def test_runner_detects_wrong_initial_argument(tmp_path):
    image = compile_ir("define i32 @main(i32 %a) { ret i32 %a }", arguments=[42])
    image.write(tmp_path)
    token_accumulator = image.manifest["stack_base"] // 8 + 1
    original = (tmp_path / "memory.hex").read_text()
    (tmp_path / "memory.hex").write_text(original.replace(
        f"@{token_accumulator:x} {42:016x}", f"@{token_accumulator:x} {41:016x}"))
    with pytest.raises(RuntimeError, match="RTL/reference mismatch"):
        run_rtl(tmp_path)
