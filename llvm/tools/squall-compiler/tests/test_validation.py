import json

import pytest

from squall_compiler.cli import asm_main, compile_main
from squall_compiler.compiler import CompileError, compile_ir
from squall_compiler.reference import main as run_main


@pytest.mark.parametrize("ir,error", [
    ("bad LLVM", "invalid LLVM IR"),
    ("define i64 @main() { ret i64 1 }", "unsupported value type"),
    ("define i32 @main(i32 %n) { %p = alloca i32, i32 %n\nret i32 0 }", "variable alloca"),
    ("define i32 @main() { %p = alloca i8\nret i32 0 }", "aligned i32/ptr cells"),
    ("define i32 @main() { %p = alloca i32\n%v = load volatile i32, ptr %p\nret i32 %v }", "volatile/atomic"),
    ("declare i32 @external()\ndefine i32 @main() { %v = call i32 @external()\nret i32 %v }", "external/indirect call"),
    ("define i32 @main() { %v = freeze i32 poison\nret i32 %v }", "unsupported instruction"),
    ("define i32 @main() { ret i32 undef }", "unsupported operand"),
    ("define i32 @main() { %p = alloca i32\n%q = getelementptr i32, ptr %p, i64 1\nret i32 0 }", "GEP indices must be i32"),
    ("@g = external global i32\ndefine i32 @main() { ret i32 0 }", "integer initializer"),
    ("@g = global i64 0\ndefine i32 @main() { ret i32 0 }", "aligned i32/ptr cells"),
    ("define i32 @other() { ret i32 0 }", "has no definition"),
    ("define i32 @main(i32 %a) { ret i32 %a }", "needs 1 arguments"),
])
def test_unsupported_ir_is_diagnostic(ir, error):
    with pytest.raises(CompileError, match=error):
        compile_ir(ir)


@pytest.mark.parametrize("options,error", [
    ({"memory_bytes": 4095}, "memory size"),
    ({"memory_bytes": 4096}, "overlaps reserved"),
    ({"frames": 0}, "positive"),
    ({"stack_tokens": 1}, "at least two tokens"),
    ({"frames": 100000}, "overlaps reserved"),
])
def test_layout_checks(options, error):
    with pytest.raises(CompileError, match=error):
        compile_ir("define i32 @main() { ret i32 0 }", **options)


def test_entry_argument_validation():
    with pytest.raises(CompileError, match="0 or 1"):
        compile_ir("define i1 @main(i1 %a) { ret i1 %a }", arguments=[2])
    with pytest.raises(CompileError, match="fit 32 bits"):
        compile_ir("define i32 @main(i32 %a) { ret i32 %a }", arguments=[1 << 32])


def test_global_pool_boundary():
    globals_ir = "\n".join(f"@g{i} = global i32 {i}" for i in range(240))
    compile_ir(globals_ir + "\ndefine i32 @main() { ret i32 0 }")
    with pytest.raises(CompileError, match="overlap the code"):
        compile_ir(globals_ir + "\n@extra = global i32 0\ndefine i32 @main() { ret i32 0 }")


def test_musttail_requires_a_separate_lowering():
    with pytest.raises(CompileError, match="musttail"):
        compile_ir("""define i32 @main(i32 %a) {
          %r = musttail call i32 @main(i32 %a)
          ret i32 %r
        }""", arguments=[0])


def test_assembly_roundtrip_and_deterministic_outputs(tmp_path):
    ir = "define i32 @main(i32 %a) { %r = mul i32 %a, -7\nret i32 %r }"
    left, right = tmp_path / "left", tmp_path / "right"
    image = compile_ir(ir, arguments=[6])
    image.write(left)
    compile_ir(ir, arguments=[6]).write(right)
    for source in left.iterdir():
        assert source.read_bytes() == (right / source.name).read_bytes()
    reassembled = tmp_path / "code.hex"
    assert asm_main([str(left / "program.sqa"), "-o", str(reassembled)]) == 0
    # Code-only assembly intentionally excludes frame/host/token initialization.
    for row in reassembled.read_text().splitlines():
        address, value = row.split()
        assert image.words[int(address[1:], 16)] == int(value, 16)
    raw = (left / "memory.bin").read_bytes()
    for address, value in image.words.items():
        assert int.from_bytes(raw[address * 8:address * 8 + 8], "little") == value


def test_command_line_failure_and_result_checks(tmp_path, capsys):
    source = tmp_path / "main.ll"
    output = tmp_path / "image"
    source.write_text("define i32 @main() { ret i32 42 }")
    assert compile_main([str(source), "-o", str(output)]) == 0
    assert run_main([str(output), "--expect", "42", "--trace"]) == 0
    assert (output / "reference.trace").read_text().startswith("T ")
    assert run_main([str(output), "--expect", "41"]) == 1
    assert "expected 41" in capsys.readouterr().err
    manifest_path = output / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["max_cycles"] = 1
    manifest_path.write_text(json.dumps(manifest))
    assert run_main([str(output)]) == 1
    assert "did not complete" in capsys.readouterr().err
    source.write_text("bad LLVM")
    assert compile_main([str(source), "-o", str(output)]) == 1
    assert "invalid LLVM IR" in capsys.readouterr().err


def test_completion_publishes_result_before_status(tmp_path):
    image = compile_ir("define i32 @main() { ret i32 42 }")
    image.write(tmp_path)
    assert run_main([str(tmp_path), "--trace", "--expect", "42"]) == 0
    executed = [int(row.split()[1], 16) for row in (tmp_path / "reference.trace").read_text().splitlines()
                if row.startswith("T ")]
    assert executed.index(image.symbols["result_write"]) < executed.index(image.symbols["status_write"])
