"""Execution and diagnostic regressions from the November compiler review."""
import json
import re

import pytest

from conftest import clang, compile_ir, load, requires_llvm, run_tool, tool
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


# --- Silicon-readiness review, November 9 -----------------------------------

NULL_CALL = """
@target = global ptr null
define i32 @main() {
  %f = load volatile ptr, ptr @target
  %r = call i32 %f(i32 3)
  ret i32 %r
}
"""


def test_null_function_pointer_traps(tmp_path, opt):
    # Word 0 is the discard destination, so a call through a null pointer
    # used to end the program with neither a result nor a status.
    image = compile_ir(tmp_path, NULL_CALL, opt=opt)
    result = run_image(image)
    assert result["quiescent"] and result["status"] == 4, result


STACK_POINTER = """
define i32 @main(i32 %word, i32 %store) {{
  %address = shl i32 %word, 2
  %p = inttoptr i32 %address to ptr
  %c = icmp ne i32 %store, 0
  br i1 %c, label %write, label %read
write:
  store volatile i32 7, ptr %p
  ret i32 1
read:
  %v = load volatile i32, ptr %p
  ret i32 1
}}
"""


@pytest.mark.parametrize("store", [0, 1])
def test_token_stack_is_not_addressable(tmp_path, opt, store):
    # The frame arena is the last region a pointer may name. A store into the
    # token stack above it overwrote a pending token, or parked forever on a
    # word with empty presence bits.
    build = tmp_path / "build"
    compile_ir(build, STACK_POINTER.format(), [0, 0], opt=opt)
    manifest = json.loads((build / "image/manifest.json").read_text())
    end = manifest["frame_arena_end_word"]
    assert end * 8 == manifest["stack_base"] - manifest["stack_token_capacity"] * 16
    last = manifest["memory_bytes"] // 8 - 1
    for index, (word, status) in enumerate([(end - 1, 1), (end, 3), (end + 1, 3),
                                             (manifest["stack_base"] // 8, 3), (last, 3)]):
        load(build / "image.o", tmp_path / f"run{index}", [word, store])
        result = run_image(tmp_path / f"run{index}")
        assert result["quiescent"] and result["status"] == status, (word, result)


def test_manifest_records_provenance_and_reset_state(tmp_path):
    compile_ir(tmp_path, "define i32 @main() { ret i32 42 }")
    manifest = json.loads((tmp_path / "image/manifest.json").read_text())
    from squall_tools.image import ISA_COMMIT
    from squall_tools.rtl import ISACOMP_COMMIT
    assert manifest["isa"]["commit"] == ISA_COMMIT == ISACOMP_COMMIT
    assert manifest["compiler"].startswith("LLVM ")
    assert manifest["reset"] == {"stack_base": manifest["memory_bytes"] - 16,
                                 "stack_top": manifest["memory_bytes"] - 32}
    # Every arena word but the root frame's return tag is an empty constant.
    memory = (tmp_path / "image/memory.bin").read_bytes()
    fill = int(manifest["frame_arena_fill"], 16).to_bytes(8, "little")
    root, end = manifest["root_frame_word"], manifest["frame_arena_end_word"]
    assert memory[(root + 1) * 8:end * 8] == fill * (end - root - 1)
    tag = int.from_bytes(memory[root * 8:root * 8 + 8], "little")
    assert tag == 2 << 62 | manifest["host_return_word"]


def test_loader_refuses_an_image_for_another_memory_size(tmp_path):
    from squall_tools.image import load_image
    compile_ir(tmp_path, "define i32 @main() { ret i32 42 }")
    tools = dict(objcopy=tool("llvm-objcopy"),
                 nm=tool("llvm-nm"))
    load_image(tmp_path / "image.o", tmp_path / "same", memory_bytes=512 * 1024, **tools)
    with pytest.raises(ValueError, match="-squall-memory-bytes=1048576"):
        load_image(tmp_path / "image.o", tmp_path / "other", memory_bytes=1024 * 1024, **tools)


def test_loader_refuses_an_image_for_another_core(tmp_path):
    # An image whose manifest names a different RTL revision (or none, as
    # images built before the field existed) is not run.
    from squall_tools.image import load_image
    compile_ir(tmp_path, "define i32 @main() { ret i32 42 }")
    section = tmp_path / "config.json"
    run_tool("llvm-objcopy", f"--dump-section=.squall.config={section}", tmp_path / "image.o",
             tmp_path / "copy.o")
    manifest = json.loads(section.read_text())
    manifest["isa"]["commit"] = "0" * 40
    section.write_text(json.dumps(manifest))
    run_tool("llvm-objcopy", f"--update-section=.squall.config={section}", tmp_path / "image.o",
             tmp_path / "other.o")
    with pytest.raises(ValueError, match="targets Isacomp 0000"):
        load(tmp_path / "other.o", tmp_path / "other")


def test_loader_refuses_unresolved_references(tmp_path):
    # llvm-mc leaves the field of a reference to an undefined label zero and
    # emits a relocation; nothing links Squall objects, so as a destination
    # the instruction would send to itself.
    from squall_tools.image import image_relocations
    source = tmp_path / "undefined.s"
    source.write_text(".section .squall.image,\"awx\",@progbits\n"
                      "dupl missing.l #0\nadd.r (1).l [4]\n")
    run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", source, "-o", tmp_path / "undefined.o")
    assert len(image_relocations(tmp_path / "undefined.o",
                                 objcopy=tool("llvm-objcopy"))) == 1
    with pytest.raises(ValueError, match="unresolved reference"):
        load(tmp_path / "undefined.o", tmp_path / "image")


@pytest.mark.parametrize("word,drains", [(2, True), (0, False)])
def test_padding_word_discards_its_token(tmp_path, word, drains):
    # Point the start token at an unused word holding the padding pattern:
    # the program must drain (with no status). The zero word that padding
    # used to be keeps its token alive forever.
    compile_ir(tmp_path, "define i32 @main() { ret i32 42 }")
    image = tmp_path / "image"
    manifest = json.loads((image / "manifest.json").read_text())
    memory = bytearray((image / "memory.bin").read_bytes())
    at = manifest["root_frame_word"] - 1
    assert memory[at * 8:at * 8 + 8] == bytes(8)
    memory[at * 8:at * 8 + 8] = word.to_bytes(8, "little")
    base = manifest["stack_base"]
    memory[base:base + 8] = (at | manifest["root_frame_word"] << 32).to_bytes(8, "little")
    (image / "memory.bin").write_bytes(memory)
    manifest["max_cycles"] = 1000
    (image / "manifest.json").write_text(json.dumps(manifest))
    result = run_image(image)
    assert result["quiescent"] == drains and result["status"] == 0, result
    assert result["cycles"] == (1 if drains else 1000), result
