"""Unsupported input is reported as a compiler error, never a crash."""
import subprocess

import pytest

from conftest import clang, requires_llvm, run_tool

pytestmark = requires_llvm


@pytest.mark.parametrize("ir,error", [
    ("define i32 @main(ptr %p) { %v = atomicrmw add ptr %p, i32 1 seq_cst\nret i32 %v }", "atomic"),
    ("define i32 @main(ptr %p) { %v = load atomic i32, ptr %p seq_cst, align 4\nret i32 %v }", "atomic"),
    ("define i32 @main(ptr %p) { %v = load float, ptr %p\nret i32 0 }", "integers or pointers"),
    ("@f = global float 1.0\ndefine i32 @main() { ret i32 0 }", "global 'f'"),
    ("declare i32 @f()\ndefine i32 @main() { %v = call i32 @f()\nret i32 %v }", "no definition"),
    ("define i32 @main(i32 %n) { %p = alloca i32, i32 %n\nret i32 0 }", "dynamic"),
    ("define i32 @main() { br label %b\nb: %p = alloca i32\nret i32 0 }", "dynamic or non-entry"),
    ("define float @main() { ret float 0.0 }", "return values"),
    ("define i128 @main() { ret i128 0 }", "return values"),
    ("define i32 @main(...) { ret i32 0 }", "variadic"),
    ("declare void @g()\ndefine ptr @main() { ret ptr @g }", "'g' has no definition"),
    ("declare void @g()\n@t = global ptr @g\ndefine i32 @main() { ret i32 0 }", "global 't'"),
    ("define i32 @main() { %v = call i32 asm \"\", \"=r\"()\nret i32 %v }", "inline assembly"),
    ("define i32 @other() { ret i32 0 }", "entry function 'main' is not defined"),
    ("define i64 @main() { ret i64 0 }", "entry function 'main' must"),
])
def test_unsupported_ir(tmp_path, ir, error):
    source = tmp_path / "input.ll"
    source.write_text(ir)
    result = run_tool("llc", "-mtriple=squallv2", "-filetype=obj", source, "-o", tmp_path / "x.o",
                      check=False)
    assert result.returncode, result.stderr
    assert "error:" in result.stderr and error in result.stderr, result.stderr
    assert "PLEASE submit" not in result.stderr and "Stack dump" not in result.stderr
    # Only the diagnostics themselves: the image is not emitted half-built.
    assert "Undefined" not in result.stderr, result.stderr


def test_clang_reports_source_locations(tmp_path):
    if not clang():
        pytest.skip("the built Squall Clang is required")
    source = tmp_path / "bad.c"
    source.write_text("float scale;\nint main(void) { return (int)scale; }\n")
    result = subprocess.run([clang(), "--target=squallv2", "-c", str(source), "-o",
                             str(tmp_path / "bad.o")], capture_output=True, text=True)
    assert result.returncode
    assert "error:" in result.stderr and "PLEASE submit" not in result.stderr, result.stderr
