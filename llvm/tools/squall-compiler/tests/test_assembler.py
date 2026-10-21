import pytest

from squall_compiler.assembler import AssemblyError, OPS, Program, assemble, slot_word, tag_word, token_words
from squall_compiler.reference import checked, reference_source, zig_executable


@pytest.mark.parametrize("record,error", [
    ("a: monadic op=Fake", "unknown opcode"),
    ("a: monadic imm=2147483648", "signed 32"),
    ("a: monadic imm=-2147483649", "signed 32"),
    ("a: dyadic addr=frame:2048", "unsigned 11"),
    ("a: dyadic addr=abs:-1", "unsigned 11"),
    ("a: monadic dest=missing.L", "undefined label"),
    ("a: monadic dest=halt.Bad", "must end"),
    ("a: monadic second=a.L", "next word"),
    ("a: write addr=536870912", "unsigned 29"),
    ("a: .slot value=2305843009213693952", "signed 62"),
    ("a: .word value=18446744073709551616", "64 bits"),
    ("a: monadic op=Add typo=3", "unknown operands"),
    ("halt: monadic", "reserved label"),
    ("a: monadic\na: monadic", "duplicate"),
    ("a: monadic imm=1 imm=2", "duplicate operand"),
    ("a:", "no record"),
    ("monadic", "needs a label"),
    ('a: monadic op="Add', "No closing quotation"),
])
def test_invalid_assembly_is_rejected(record, error):
    with pytest.raises(AssemblyError, match=error):
        assemble(record)


def test_signed_destination_and_code_slot_ranges():
    with pytest.raises(AssemblyError, match="signed 20"):
        assemble("a: monadic dest=halt.L", origin=1 << 19 | 1)
    program = Program()
    program.add("a", "dyadic", addr="code:last")
    for index in range(1023):
        program.add(f"fill{index}", ".word", value=0)
    program.add("last", ".slot", value=1)
    with pytest.raises(AssemblyError, match="signed 11"):
        assemble(program)


def test_tokens_use_word_addresses_and_little_endian_words():
    low, high = token_words(3, 5, "R", -7)
    assert low == (3 | 5 << 29 | 1 << 58) << 2
    assert high == (1 << 64) - 7
    assert tag_word(3, 5, "R") << 2 == low
    assert slot_word(-1) == 0xbfffffffffffffff


def test_encodings_match_upstream_zig_assembler(tmp_path):
    # These use the upstream assembler's semantic API and packed structs. The
    # probe contains no Python bit packing or decoder that could share a bug.
    records = []
    entries = []
    for index, op in enumerate(OPS):
        label = f"op{index}"
        records.append(f"{label}: monadic op={op} imm=-2147483648 dest=last.R")
        entries.append(f'.{{ .label = "{label}", .value = .{{ .monadic = .{{ '
                       f'.op = .{op}, .imm = .{{ .literal = -2147483648 }}, .d = .r("last") }} }} }}')
    records += [
        "both: monadic op=Add imm=7 dest=op0.L second=frame.R",
        "frame: dyadic op=Sub addr=frame:2047 match=Once dest=both.R second=code.L",
        "code: dyadic op=Mul addr=code:last match=Const dest=frame.L",
        "absolute: dyadic op=Dupr addr=abs:17 match=Read dest=halt.L",
        "extract: extract target=op0.R addr=frame:0 match=Read dest=send.L",
        "send: send addr=code:op0 match=Read dest=halt.L",
        "write: write addr=1234",
        "last: .slot value=-7",
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
    source = '''const std = @import("std");
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
'''.replace("ENTRIES", ",\n".join(entries))
    probe = tmp_path / "encoding.zig"
    binary = tmp_path / "encoding"
    probe.write_text(source)
    reference = reference_source()
    checked([zig_executable(), "build-exe", "-lc", "-O", "ReleaseSafe", "--dep", "squall",
             f"-Mroot={probe}", f"-Msquall={reference / 'src/root.zig'}", f"-femit-bin={binary}"], timeout=120)
    expected = [int(line, 16) for line in checked([str(binary)]).splitlines()]
    words, _ = assemble("\n".join(records), origin=2)
    assert list(words.values()) == expected
    # The new public spelling must retain the upstream semantic encodings.
    assert list(assemble(Program.parse("\n".join(records)).text(), origin=2)[0].values()) == expected
