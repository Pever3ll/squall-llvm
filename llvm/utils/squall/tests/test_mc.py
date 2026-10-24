"""llvm-mc encodings checked against the upstream Zig assembler."""
import pytest

from conftest import requires_llvm, run_tool
from squall_tools.reference import checked, reference_source, zig_executable

pytestmark = requires_llvm

# Upstream enum names; the team syntax uses their lowercase spelling.
OPS = ["Nop", "Add", "Sub", "Mul", "Div", "Divu", "Rem", "Mod", "And", "Or", "Xor", "Shl",
       "Shrl", "Shra", "Not", "Rev", "Dupl", "Dupr", "Beqz", "Beql", "Beqr"]


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
        "frame: sub both.r.l [2047]",
        "code: mul.c frame.l (last)",
        "absolute: dupr.r halt.l 17",
        "extract: extract.r send.l op0.r [0]",
        "send: send.r halt.l (op0)",
        "write: write 1234",
        "last: .slot -7",
    ]
    entries += [
        '.{ .label = "both", .value = .{ .monadic = .{ .op = .Add, .imm = .{ .literal = 7 }, .d = .l("op0"), .second = .{ .label = "frame", .port = .Right } } } }',
        '.{ .label = "frame", .value = .{ .dyadic = .{ .slot = .{ .frame_words = 2047 }, .matching = .Once, .form = .{ .arith = .{ .op = .Sub, .second = .{ .label = "code" } } }, .d = .r("both") } } }',
        '.{ .label = "code", .value = .{ .dyadic = .{ .slot = .{ .code = "last" }, .matching = .Const, .form = .{ .arith = .{ .op = .Mul } }, .d = .l("frame") } } }',
        '.{ .label = "absolute", .value = .{ .dyadic = .{ .slot = .{ .absolute_addr = 136 }, .matching = .Read, .form = .{ .arith = .{ .op = .Dupr } }, .d = .halt } } }',
        '.{ .label = "extract", .value = .{ .dyadic = .{ .slot = .{ .frame_words = 0 }, .matching = .Read, .form = .{ .extract = .r("op0") }, .d = .l("send") } } }',
        '.{ .label = "send", .value = .{ .dyadic = .{ .slot = .{ .code = "op0" }, .matching = .Read, .form = .send, .d = .halt } } }',
        '.{ .label = "write", .value = .{ .write = .{ .target = .{ .addr = 9872 } } } }',
        '.{ .label = "last", .value = .{ .value = .{ .pb = .Constant, .val = @bitCast(@as(i62, -7)) } } }',
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
    assert words == expected

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


def test_forward_and_backward_labels_resolve_without_relocations(tmp_path):
    obj, words = text_section(tmp_path, "top: add later.l #1\nlater: dupl top.r #0\n")
    assert run_tool("llvm-readobj", "-r", obj).stdout.count("R_SQUALL") == 0
    assert (words[0] >> 43) & 0xfffff == 1 and (words[1] >> 43) & 0xfffff == 0xfffff


@pytest.mark.parametrize("text,error", [
    ("add.r (1).l [2048]", "out of range"),
    ("add (1).l #2147483648", "out of range"),
    ("add (524288).l #0", "out of range"),
    ("write -1", "out of range"),
    ("send.r (1).l.r [0]", "secondary"),
    ("extract.r (1).l (1).l.r [0]", "secondary"),
    ("add.x (1).l [0]", "matching suffix"),
    ("add.r (1).l #0", "immediates"),
    (".slot 2305843009213693952", "62 bits"),
    ("frob (1).l #0", "unknown Squall instruction"),
    ("add (1).l.l r #0", "duplicate secondary"),
])
def test_invalid_assembly_is_diagnosed(tmp_path, text, error):
    source = tmp_path / "invalid.s"
    source.write_text(text + "\n")
    result = run_tool("llvm-mc", "-triple=squallv2", "-filetype=obj", source, "-o", tmp_path / "x.o",
                      check=False)
    assert result.returncode and "error:" in result.stderr and error in result.stderr, result.stderr
