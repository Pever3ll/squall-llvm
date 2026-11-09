"""llvm-mc encodings checked against the upstream Zig assembler and Isacomp's sqasm."""
import os
from pathlib import Path
import sys

import pytest

from conftest import requires_llvm, run_tool
from squall_tools.reference import checked, reference_source, zig_executable

pytestmark = requires_llvm

# Upstream enum names; the team syntax uses their lowercase spelling.
OPS = ["Nop", "Add", "Sub", "Mul", "Div", "Divu", "Rem", "Mod", "And", "Or", "Xor", "Shl",
       "Shrl", "Shra", "Not", "Rev", "Dupl", "Dupr", "Beqz", "Beql", "Beqr", "Bler", "Blel"]


def text_section(tmp_path, source, name="program"):
    path, obj, binary = (tmp_path / f"{name}{suffix}" for suffix in (".s", ".o", ".bin"))
    path.write_text(source)
    run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", path, "-o", obj)
    run_tool("llvm-objcopy", f"--dump-section=.text={binary}", obj, tmp_path / f"{name}-copy.o")
    data = binary.read_bytes()
    return obj, [int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data), 8)]


def test_encodings_match_upstream_zig_assembler(tmp_path):
    # The Zig probe uses the upstream assembler's semantic API; neither side
    # shares bit-packing code with the other.
    records, entries = [], []
    for index, op in enumerate(OPS):
        records.append(f"op{index}: {op.lower()} last.r #-2147483648")
        entries.append(f'.{{ .label = "op{index}", .value = .{{ .monadic = .{{ '
                       f'.op = .{op}, .imm = .{{ .literal = -2147483648 }}, .d = .r("last") }} }} }}')
    records += [
        "both: add op0.l.r #7",
        "frame: sub both.r.l [4095]",
        "code: mul.c frame.l (last)",
        "absolute: dupr.r halt.l 17",
        "extract: extract send.l op0.r",
        "send: send.r halt.l (op0)",
        "write: write 1234",
        "masked: write.a op0.r 1235 0x0f",
        "last: .slot 7",
    ]
    entries += [
        '.{ .label = "both", .value = .{ .monadic = .{ .op = .Add, .imm = .{ .literal = 7 }, .d = .l("op0"), .second = .{ .label = "frame", .port = .Right } } } }',
        '.{ .label = "frame", .value = .{ .dyadic = .{ .slot = .{ .frame_words = 4095 }, .matching = .Once, .form = .{ .arith = .{ .op = .Sub, .second = .{ .label = "code" } } }, .d = .r("both") } } }',
        '.{ .label = "code", .value = .{ .dyadic = .{ .slot = .{ .code = "last" }, .matching = .Const, .form = .{ .arith = .{ .op = .Mul } }, .d = .l("frame") } } }',
        '.{ .label = "absolute", .value = .{ .dyadic = .{ .slot = .{ .absolute_addr = 136 }, .matching = .Read, .form = .{ .arith = .{ .op = .Dupr } }, .d = .halt } } }',
        '.{ .label = "extract", .value = .{ .extract = .{ .d = .l("send"), .mint = .r("op0") } } }',
        '.{ .label = "send", .value = .{ .dyadic = .{ .slot = .{ .code = "op0" }, .matching = .Read, .form = .send, .d = .halt } } }',
        '.{ .label = "write", .value = .{ .write = .{ .target = .{ .addr = 9872 } } } }',
        '.{ .label = "masked", .value = .{ .write = .{ .target = .{ .addr = 9880 }, .ack = .r("op0"), .byte_enables = 0x0f } } }',
        '.{ .label = "last", .value = .{ .value = .{ .pb = .Constant, .val = 7 } } }',
    ]
    probe = tmp_path / "encoding.zig"
    probe.write_text('''const std = @import("std");
const sq = @import("squall");
extern "c" fn printf([*:0]const u8, ...) c_int;
pub fn main(init: std.process.Init) !void {
    const entries: []const sq.assembler.Entry = &.{ ENTRIES };
    var state = try sq.assembler.create_image(init.gpa, entries, &.{}, 0x10000);
    defer state.memory.deinit();
    for (entries, 0..) |_, i| {
        const word: u64 = @bitCast(state.read(u64, @intCast(0x10 + i * 8)));
        _ = printf("%llx\\n", word);
    }
}
'''.replace("ENTRIES", ",\n".join(entries)))
    binary = tmp_path / "encoding"
    checked([zig_executable(), "build-exe", "-lc", "-O", "ReleaseSafe", "--dep", "squall",
             f"-Mroot={probe}", f"-Msquall={reference_source() / 'src/root.zig'}",
             f"-femit-bin={binary}"], timeout=180)
    expected = [int(line, 16) for line in checked([str(binary)]).splitlines()]
    # The Zig image places the first entry at word 2; halt is word 0.
    obj, words = text_section(tmp_path, ".squall_origin 2\n" + "\n".join(records) + "\n")
    # Extract's pad (bits 41:31) is read by neither the RTL nor the
    # interpreter, and the upstream Zig image does not always leave it zero.
    # llvm-mc always does.
    pad = ((1 << 11) - 1) << 31
    at = OPS.__len__() + 4  # The "extract" record.
    assert words[at] & pad == 0
    assert [w & ~pad if i == at else w for i, w in enumerate(words)] == \
        [w & ~pad if i == at else w for i, w in enumerate(expected)]

    # The disassembler and printer reproduce the same instruction words; the
    # final `.slot` is data.
    instructions = words[:-1]
    listing = run_tool("llvm-mc", "-triple=squallv2", "-disassemble",
                       input=" ".join(f"0x{b:02x}" for w in instructions
                                      for b in w.to_bytes(8, "little"))).stdout
    lines = [line for line in listing.splitlines() if line.strip() and not line.lstrip().startswith(".")]
    assert len(lines) == len(instructions)
    _, again = text_section(tmp_path, ".squall_origin 2\n" + "\n".join(lines) + "\n", "again")
    assert again == instructions


def test_separated_secondary_port_spelling(tmp_path):
    _, a = text_section(tmp_path, "add.r next.l.r [4]\nnext: write 1\n", "a")
    _, b = text_section(tmp_path, "add.r next.l r [4]\nnext: write 1\n", "b")
    assert a == b


def test_dotted_labels_keep_components_that_are_not_ports(tmp_path):
    # "loop.x.l" is label "loop.x" with port .l; "next.x.l.r" adds a secondary.
    _, words = text_section(tmp_path, "loop.x: add loop.x.l #1\nnext.x: add.r next.x.l.r [4]\n")
    assert (words[0] >> 43) & 0xfffff == 0 and words[0] >> 63 == 0
    assert (words[1] >> 43) & 0xfffff == 0 and (words[1] >> 8) & 3 == 0b11


def test_alignment_padding_discards_a_token(tmp_path):
    # No padding word could pass on a token arriving on either port with its
    # value and port intact, so padding never forwards one. A token reaching
    # it is discarded: each word is `write 0 0x00`, which stores no byte and
    # forms no token. (A zero word would be `nop (0).l #0`, which sends the
    # token to itself forever.)
    obj, words = text_section(tmp_path, "start: dupr (1).r #123\n.p2align 6\nnext: sub.c (2).l [0]\n")
    assert words[1:8] == [2] * 7, [hex(w) for w in words]
    for word in words[1:8]:
        assert word & 3 == 2            # Write form,
        assert (word >> 31) & 0xff == 0  # no byte enabled,
        assert (word >> 39) & 1 == 0     # no acknowledgement token.
    assert run_tool("llvm-objdump", "-d", obj).stdout.count("write 0 0x00") == 7


def test_forward_and_backward_labels_resolve_without_relocations(tmp_path):
    obj, words = text_section(tmp_path, "top: add later.l #1\nlater: dupl top.r #0\n")
    assert run_tool("llvm-readobj", "-r", obj).stdout.count("R_SQUALL") == 0
    assert (words[0] >> 43) & 0xfffff == 1 and (words[1] >> 43) & 0xfffff == 0xfffff


@pytest.mark.parametrize("text,error", [
    ("add.r (1).l [4096]", "out of range"),
    ("add.r (1).l (2048)", "out of range"),
    ("write 1 256", "out of range"),
    ("write.x 1", "suffix must be .a"),
    ("extract.r (1).l (1).l", "no matching suffix"),
    ("add (1).l #2147483648", "out of range"),
    ("add (524288).l #0", "out of range"),
    ("write -1", "out of range"),
    ("send.r (1).l.r [0]", "secondary"),
    ("extract (1).l (1).l.r", "secondary"),
    ("add.x (1).l [0]", "matching suffix"),
    ("add.r (1).l #0", "immediates"),
    (".slot 4611686018427387904", "62 bits"),
    (".slot -7", "unsigned 62 bits"),
    ("frob (1).l #0", "unknown Squall instruction"),
    ("add (1).l.l r #0", "duplicate secondary"),
])
def test_invalid_assembly_is_diagnosed(tmp_path, text, error):
    source = tmp_path / "invalid.s"
    source.write_text(text + "\n")
    result = run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", source, "-o", tmp_path / "x.o",
                      check=False)
    assert result.returncode and "error:" in result.stderr and error in result.stderr, result.stderr


ISACOMP = os.environ.get("SQUALL_ISACOMP_DIR")


@pytest.mark.skipif(not ISACOMP, reason="set SQUALL_ISACOMP_DIR to cross-check Isacomp's sqasm")
def test_encodings_match_isacomp_sqasm(tmp_path):
    """The RTL repository's own encoder (isa/squall/tools/sqasm.py) is the
    other assembler for this ISA; the two must agree bit for bit."""
    sys.path.insert(0, str(Path(ISACOMP) / "isa/squall/tools"))
    try:
        import sqasm as S
    finally:
        sys.path.pop(0)
    cases = [
        ("add (3).r #-5", S.monadic(S.ADD, d_off=3, d_port=S.R, imm=-5)),
        ("divu (2).l.r #7", S.monadic(S.DIVU, d_off=2, d_port=S.L, imm=7, second=S.R)),
        ("bler (-1).l #9", S.monadic(S.BLER, d_off=-1, imm=9)),
        ("blel (1).r.l [5]", S.dy_arith(S.BLEL, addr=5, addr_tag=S.FRAME, d_off=1, d_port=S.R,
                                         second=S.L)),
        ("extract (1).l (3).r", S.extract(d2_off=3, d2_port=S.R, d_off=1, d_port=S.L)),
        ("sub.c (2).l (-3)", S.dy_arith(S.SUB, addr=-3, addr_tag=S.CODE, matching=S.CONST, d_off=2)),
        ("add.r (1).l 4095", S.dy_arith(S.ADD, addr=4095, addr_tag=S.ABS, matching=S.READ, d_off=1)),
        ("send (1).r [10]", S.dy_send(addr=10, addr_tag=S.FRAME, matching=S.ONCE, d_off=1, d_port=S.R)),
        ("write 65", S.write(65)),
        ("write 112 0x0f", S.write(112, be=0x0f)),
        ("write.a (1).r 112 0xf0", S.write(112, be=0xf0, ack_off=1, ack_port=S.R)),
    ]
    _, words = text_section(tmp_path, "\n".join(text for text, _ in cases) + "\n", "sqasm")
    assert [hex(w) for w in words] == [hex(v & ((1 << 64) - 1)) for _, v in cases]
