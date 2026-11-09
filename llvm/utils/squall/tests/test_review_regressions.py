"""Execution and diagnostic regressions from the November compiler review."""
import re

import pytest

from conftest import clang, compile_ir, load, requires_llvm, run_tool
from squall_tools.reference import run_image

pytestmark = requires_llvm


def frame_accesses(image, source, opt, options=()):
    """Observe source objects' reads/writes, even when the result is unchanged."""
    mir = run_tool("llc", "-mtriple=squallv2", opt, *options,
                   "-stop-after=prolog-epilog", source, "-o", "-").stdout
    slots = {int(slot): obj for slot, obj in re.findall(
        r'(?:STF [^,]+,|LDF) (\d+), 0 :: \(volatile[^\n]*%ir\.([a-z]+)', mir)}
    memory = (image / "memory.bin").read_bytes()
    result = run_image(image, trace=True)
    assert result["status"] == 1 and result["quiescent"], result
    events = []
    for line in (image / "reference.trace").read_text().splitlines():
        if not line.startswith("T "):
            continue
        ip, fp, port, value = (int(v, 16) for v in line.split()[1:])
        word = int.from_bytes(memory[ip * 8:ip * 8 + 8], "little")
        # Dyadic Arith (bit 42 clear), frame-relative address (39:38), and
        # the Read/Const rule (41:40); the slot is 37:26.
        if word & 3 == 1 and not (word >> 42) & 1 and (word >> 38) & 3 == 0:
            slot, rule = (word >> 26) & 4095, (word >> 40) & 3
            if slot in slots:
                events.append((slots[slot], rule, value))
    assert slots, mir
    return events


def test_volatile_load_is_executed(tmp_path, opt):
    image = compile_ir(tmp_path, """define i32 @main() {
      %x = alloca i32
      store volatile i32 123, ptr %x
      %v = load volatile i32, ptr %x
      ret i32 %v
    }""", opt=opt)
    events = frame_accesses(image, tmp_path / "input.ll", opt)
    assert [(obj, rule) for obj, rule, _ in events] == [("x", 1), ("x", 2)]


@pytest.mark.parametrize("fork", [False, True])
def test_volatile_store_order(tmp_path, opt, fork):
    options = ("-squall-fork-join",) if fork else ()
    image = compile_ir(tmp_path, """define i32 @main() {
      %a = alloca i32
      %b = alloca i32
      store volatile i32 11, ptr %a
      store volatile i32 22, ptr %b
      store volatile i32 33, ptr %a
      store volatile i32 44, ptr %b
      ret i32 42
    }""", opt=opt, options=options)
    assert frame_accesses(image, tmp_path / "input.ll", opt, options) == [
        ("a", 1, 11), ("b", 1, 22), ("a", 1, 33), ("b", 1, 44)]


@pytest.mark.parametrize("name", ["f.x", "f.l", "f.r", "f.l.r"])
def test_port_like_function_symbols_roundtrip(tmp_path, opt, name):
    ir = f"""@table = global ptr @{name}
    define i32 @{name}(i32 %x) noinline {{
      %r = add i32 %x, 7
      ret i32 %r
    }}
    define i32 @main(i32 %x) {{
      %direct = call i32 @{name}(i32 %x)
      %fp = load volatile ptr, ptr @table
      %indirect = call i32 %fp(i32 %x)
      %r = add i32 %direct, %indirect
      ret i32 %r
    }}"""
    image = compile_ir(tmp_path, ir, [35], opt=opt)
    assert run_image(image)["result"] == 84
    assert_roundtrip(tmp_path, opt, [35], 84)


def assert_roundtrip(tmp_path, opt, arguments, expected):
    """input.ll through llc -S and llvm-mc gives the same image, and runs."""
    asm, obj = tmp_path / "image.s", tmp_path / "roundtrip.o"
    run_tool("llc", "-mtriple=squallv2", opt, tmp_path / "input.ll", "-o", asm)
    run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", asm, "-o", obj)
    for original, binary in ((tmp_path / "image.o", tmp_path / "direct.bin"),
                             (obj, tmp_path / "roundtrip.bin")):
        run_tool("llvm-objcopy", f"--dump-section=.squall.image={binary}",
                 original, tmp_path / "copy.o")
    assert (tmp_path / "direct.bin").read_bytes() == (tmp_path / "roundtrip.bin").read_bytes()
    load(obj, tmp_path / "roundtrip", arguments)
    result = run_image(tmp_path / "roundtrip")
    assert result["status"] == 1 and result["result"] == expected, result


@pytest.mark.parametrize("kind", ["global", "function"])
def test_halt_symbols_roundtrip(tmp_path, opt, kind):
    # llvm-mc predefines `halt` only when the file does not define it.
    if kind == "global":
        ir = """@halt = global i32 5
        define i32 @main(i32 %x) {
          %v = load volatile i32, ptr @halt
          %r = add i32 %x, %v
          ret i32 %r
        }"""
    else:
        ir = """define i32 @halt(i32 %x) noinline {
          %r = add i32 %x, 5
          ret i32 %r
        }
        define i32 @main(i32 %x) {
          %r = call i32 @halt(i32 %x)
          ret i32 %r
        }"""
    image = compile_ir(tmp_path, ir, [37], opt=opt)
    assert run_image(image)["result"] == 42
    assert_roundtrip(tmp_path, opt, [37], 42)


@pytest.mark.parametrize("size", [4294967295, 4294967296, 4294967297, 8589934592])
def test_wide_alloca_elements(tmp_path, opt, size):
    # A zero count is valid; no nonzero count fits in the allocation area.
    ir = f"""@sink = global ptr null
    define i32 @main(i32 %n) {{
      %p = alloca [{size} x i8], i32 %n, align 4
      store volatile ptr %p, ptr @sink
      ret i32 42
    }}"""
    image = compile_ir(tmp_path, ir, [0], opt=opt)
    result = run_image(image)
    assert result["status"] == 1 and result["result"] == 42, result
    load(tmp_path / "image.o", tmp_path / "nonzero", [1])
    result = run_image(tmp_path / "nonzero")
    assert result["status"] == 4 and result["quiescent"], result


@pytest.mark.parametrize("align", [2147483648, 4294967296])
@pytest.mark.parametrize("count", [0, 1])
def test_huge_alloca_alignment(tmp_path, opt, align, count):
    # No address in the allocation area has such alignment: always a trap.
    ir = f"""@sink = global ptr null
    define i32 @main(i32 %n) {{
      %p = alloca i8, i32 %n, align {align}
      store volatile ptr %p, ptr @sink
      ret i32 42
    }}"""
    image = compile_ir(tmp_path, ir, [count], opt=opt)
    result = run_image(image)
    assert result["status"] == 4 and result["quiescent"], result


@pytest.mark.parametrize("param, value, expected", [
    ("i8 zeroext", -1, 255), ("i8 zeroext", 200, 200),
    ("i8 signext", 255, -1), ("i8 signext", -128, -128),
    ("i16 zeroext", -1, 65535), ("i16 signext", 65535, -1),
    ("i8", 255, 255), ("i1 signext", 1, -1), ("i1 zeroext", 1, 1)])
def test_narrow_entry_arguments(tmp_path, opt, param, value, expected):
    # The loader converts as C does; the entry trusts the extension attribute.
    ty, *attribute = param.split()
    extend = "sext" if attribute == ["signext"] else "zext"
    image = compile_ir(tmp_path, f"""define i32 @main({param} %x) {{
      %r = {extend} {ty} %x to i32
      ret i32 %r
    }}""", [value], opt=opt)
    result = run_image(image)
    assert result["status"] == 1 and result["result"] == expected, result


@pytest.mark.parametrize("param, value", [("i8 zeroext", 256), ("i8 signext", -129),
                                          ("i16 zeroext", 65536)])
def test_narrow_entry_argument_range(tmp_path, param, value):
    with pytest.raises(ValueError, match="does not fit"):
        compile_ir(tmp_path, f"""define i32 @main({param} %x) {{
          ret i32 0
        }}""", [value])


@pytest.mark.parametrize("name", ["my kernel", "tab\tname", " lead", "trail "])
def test_entry_names_with_whitespace_load(tmp_path, opt, name):
    # The loader reads the entry's word from llvm-nm, whose name column is
    # the rest of the line.
    image = compile_ir(tmp_path, f'define i32 @"{name}"() {{ ret i32 42 }}',
                       entry=name, opt=opt)
    result = run_image(image)
    assert result["status"] == 1 and result["result"] == 42, result


@pytest.mark.parametrize("linkage", ["weak", "weak_odr", "linkonce", "linkonce_odr"])
def test_weak_entry_loads(tmp_path, opt, linkage):
    # llvm-nm marks a defined weak function W, not T.
    image = compile_ir(tmp_path, f"define {linkage} i32 @main() {{ ret i32 42 }}",
                       opt=opt)
    result = run_image(image)
    assert result["status"] == 1 and result["result"] == 42, result


@pytest.mark.parametrize("count", ["", ", i32 %n"])
def test_scalable_alloca_is_diagnosed(tmp_path, opt, count):
    source = tmp_path / "scalable.ll"
    source.write_text(f"""define i32 @main(i32 %n) {{
      %p = alloca <vscale x 4 x i32>{count}
      store volatile i32 0, ptr %p
      ret i32 0
    }}""")
    result = run_tool("llc", "-mtriple=squallv2", "-filetype=obj", opt,
                      source, "-o", tmp_path / "scalable.o", check=False)
    assert result.returncode != 0
    assert "scalable vectors are not supported on Squall" in result.stderr
    assert "PLEASE submit" not in result.stderr and "Stack dump" not in result.stderr


def test_musttail_is_diagnosed(tmp_path, opt):
    source = tmp_path / "tail.ll"
    source.write_text("""define i32 @main(i32 %n) noinline optnone {
      %r = musttail call i32 @main(i32 %n)
      ret i32 %r
    }""")
    result = run_tool("llc", "-mtriple=squallv2", "-filetype=obj", opt,
                      source, "-o", tmp_path / "tail.o", check=False)
    assert result.returncode != 0
    assert "musttail calls are not supported on Squall" in result.stderr
    assert "PLEASE submit" not in result.stderr and "Stack dump" not in result.stderr


def test_c_musttail_is_diagnosed(tmp_path, opt):
    if not clang():
        pytest.skip("the built Squall Clang is required")
    source = tmp_path / "tail.c"
    source.write_text("""__attribute__((noinline,optnone)) int down(int n) {
      if (n == 0) return 42;
      [[clang::musttail]] return down(n - 1);
    }
    int main(void) { return down(10000); }
    """)
    result = run_tool("clang", "--target=squallv2", opt, source,
                      "-o", tmp_path / "tail.o", check=False)
    assert result.returncode != 0
    assert "musttail calls are not supported on Squall" in result.stderr
    assert "PLEASE submit" not in result.stderr and "Stack dump" not in result.stderr

