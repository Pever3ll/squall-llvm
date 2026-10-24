// The interpreter itself is the pinned upstream module, not a reimplementation.
const std = @import("std");
const sq = @import("squall");
const FILE = opaque {};
extern "c" fn fopen([*:0]const u8, [*:0]const u8) ?*FILE;
extern "c" fn fclose(*FILE) c_int;
extern "c" fn fread([*]u8, usize, usize, *FILE) usize;
extern "c" fn ferror(*FILE) c_int;
extern "c" fn printf([*:0]const u8, ...) c_int;

pub fn main(init: std.process.Init) !void {
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    if (args.len != 9 and args.len != 10) return error.BadArguments;
    const file_name = try init.arena.allocator().dupeZ(u8, args[1]);
    const file = fopen(file_name, "rb") orelse return error.ImageOpenFailed;
    defer _ = fclose(file);
    var state: sq.State = .{
        .memory = .init(init.gpa),
        .stackBase = try std.fmt.parseInt(u32, args[2], 10),
        .stackTop = try std.fmt.parseInt(u32, args[3], 10),
    };
    defer state.memory.deinit();
    const max_cycles = try std.fmt.parseInt(usize, args[4], 10);
    const result_addr = try std.fmt.parseInt(u32, args[5], 10);
    const status_addr = try std.fmt.parseInt(u32, args[6], 10);
    const capacity = try std.fmt.parseInt(u64, args[7], 10);
    const token_capacity = try std.fmt.parseInt(u32, args[8], 10);
    const stack_low = state.stackBase - token_capacity * 16;
    var peak_tokens: u32 = (state.stackBase - state.stackTop) / 16;
    var buffer: [8192]u8 = undefined;
    var at: usize = 0;
    while (true) {
        const count = fread(&buffer, 1, buffer.len, file);
        if (count == 0) break;
        if (at + count > capacity) return error.ImageTooLarge;
        for (buffer[0..count], 0..) |byte, i| {
            if (byte != 0) try state.memory.put(@intCast(at + i), byte);
        }
        at += count;
    }
    if (ferror(file) != 0 or at != capacity) return error.BadImageRead;
    var cycles: usize = 0;
    while (cycles < max_cycles and !state.quiescent()) : (cycles += 1) {
        if (args.len == 10) {
            const token: sq.Token = @bitCast(state.read(sq.Token, state.stackTop + 16));
            _ = printf("T %llx %llx %llx %llx\n", @as(u64, token.tag.ip.repr),
                @as(u64, token.tag.fp.repr), @as(u64, @intFromEnum(token.tag.port)), token.acc);
        }
        sq.clock(&state);
        if (state.stackTop < stack_low or state.stackTop > state.stackBase)
            return error.TokenStackOverflow;
        peak_tokens = @max(peak_tokens, (state.stackBase - state.stackTop) / 16);
    }
    const value: u64 = @bitCast(state.read(u64, result_addr));
    const status: u64 = @bitCast(state.read(u64, status_addr));
    if (args.len == 10) {
        var address: u64 = 0;
        while (address < capacity) : (address += 8) {
            const word: u64 = @bitCast(state.read(u64, @intCast(address)));
            if (word != 0) _ = printf("M %llx %llx\n", address / 8, word);
        }
    }
    _ = printf("{\"result_bits\":%llu,\"status\":%llu,\"cycles\":%llu,\"peak_tokens\":%llu,\"quiescent\":%s}\n",
        value, status, @as(u64, cycles), @as(u64, peak_tokens),
        if (state.quiescent()) @as([*:0]const u8, "true") else "false");
}
