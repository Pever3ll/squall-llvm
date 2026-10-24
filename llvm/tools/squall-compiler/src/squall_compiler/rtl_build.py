"""Isolated child process for generating pinned Isacomp RTL and its harness.

The harness adapts signal names and reset/memory interfaces only. Processor RTL
comes unchanged from the pinned generators. It never patches processor logic.
"""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys


def retarget(tb, rtl):
    held = [name for name in ("mem_ext_stall", "PIPE_Decode_stall_ctrl", "PIPE_f_stall_ctrl")
            if re.search(rf"\b{name}\b", rtl)]
    expression = " || ".join(f"dut.{name}" for name in held) or "1'b0"
    tb = re.sub(r"`define TB_STALL_EXPR .*", f"`define TB_STALL_EXPR ({expression})", tb, count=1)
    if not re.search(r"\bmemory_tok_at_rd\b\s*;", rtl):
        replacement = None
        if "tok_tag_word = stack_data_0" in rtl:
            replacement = "stack_data_0"
        elif re.search(r"\bmemory_tok_at_rd_low\b\s*;", rtl):
            replacement = "memory_tok_at_rd_low"
        if replacement:
            tb = re.sub(r"\bmemory_tok_at_rd\b(?!_)", replacement, tb)
    return tb


def memory_stalls(tb, latency):
    """Add reproducible per-port response delays, preserving held responses.

    Only the external-memory model is changed. Poisoned unready read data and
    upstream write ordering remain intact. Jitter is selected at runtime.
    """
    if not latency:
        return tb
    declarations = []
    ports = re.findall(r"reg \[7:0\] (\w+)_(wc|c);", tb)
    for index, (name, counter) in enumerate(ports):
        declarations.append(f"    reg [31:0] {name}_{counter}_rng;\n    reg [8:0] {name}_{counter}_delay;")
        needle = f"if ({name}_{counter} >= {latency})"
        if tb.count(needle) != 1:
            raise RuntimeError("external-memory delay model no longer matches upstream")
        tb = tb.replace(needle, f"if ({name}_{counter} >= {name}_{counter}_delay)")
        # The counter's two reset sites are the reset branch and the
        # withdrawn/changed-address branch. Drawing while idle is deterministic
        # and gives each new request a stable delay until its response is held.
        reset = f"{name}_{counter} <= 0;"
        reset_branch = (reset + f" {name}_{counter}_rng <= stall_seed ^ 32'd{index + 1};"
                        f" {name}_{counter}_delay <= {latency};")
        idle_branch = (reset + f" {name}_{counter}_rng <= stall_hash({name}_{counter}_rng);"
                       f" {name}_{counter}_delay <= {latency} + (stall_hash({name}_{counter}_rng) % (stall_jitter + 1));")
        if tb.count(reset) != 2:
            raise RuntimeError("external-memory counter reset model changed")
        tb = tb.replace(reset, reset_branch, 1)
        start = tb.index(reset_branch) + len(reset_branch)
        tb = tb[:start] + tb[start:].replace(reset, idle_branch, 1)
    if not ports:
        raise RuntimeError("production memory model has no delay counters")
    tb = re.sub(r"reg \[7:0\] (\w+_(?:wc|c));", r"reg [8:0] \1;", tb)
    declarations.append("""    integer stall_seed;
    integer stall_jitter;
    initial begin
        if (!$value$plusargs("stall_seed=%d", stall_seed)) stall_seed = 0;
        if (!$value$plusargs("stall_jitter=%d", stall_jitter)) stall_jitter = 0;
        if (stall_jitter < 0 || stall_jitter > 200) $fatal(1, "invalid stall jitter");
        $display("STALL_CONFIG %0d %0d", stall_seed, stall_jitter);
    end
    function automatic [31:0] stall_hash(input [31:0] value);
        reg [31:0] x;
        begin
            x = value ^ (value << 13);
            x = x ^ (x >> 17);
            stall_hash = x ^ (x << 5);
        end
    endfunction
""")
    return tb.replace("    processor dut (", "\n".join(declarations) + "\n    processor dut (", 1)


def compile_command(command, log, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=300)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"RTL compilation failed; inspect {log}:\n{result.stderr[-4000:]}")


def main():
    root, output, variant, latency = sys.argv[1:]
    root, output, latency = Path(root).resolve(), Path(output).resolve(), int(latency)
    sys.path[:0] = [str(root), str(root / "test"), str(root / "isa/squall/generators")]
    output.mkdir(parents=True, exist_ok=True)
    from squall_fixtures import tie_off_resets
    from create_squall_pipeline import pipelined
    from squall_build import build, verilog

    with contextlib.redirect_stdout(io.StringIO()) as generation:
        if variant == "production":
            # Use the actual production generator, including its precise pass
            # order and default cache geometries; do not recreate a lookalike.
            from create_squall_prod import main as production_main
            if production_main(["-o", str(output / "processor.v")]) not in (None, 0):
                raise RuntimeError("production generator failed")
            rtl, depth = (output / "processor.v").read_text(), 6
        else:
            graph = build(quiet=True) if variant == "baseline" else pipelined("full", "stall", quiet=True)
            rtl = verilog(graph, quiet=True)
            depth = (graph.get_max_pipe_time() or 0) + 1
    (output / "generate.log").write_text(generation.getvalue())
    (output / "processor.v").write_text(rtl)
    tb = retarget((root / "isa/squall/squall_v2_tb.v").read_text(), rtl)
    # The original bench prints DRAINED before caches are flushed. Add explicit
    # failure assertions and a completion marker after flush/quiescence.
    if variant == "production":
        from extmem_tb import build_tb, QUIESCE_LIMIT
        tb = memory_stalls(build_tb(rtl, tb, latency), latency)
        tb = tb.replace('        $display("FLUSH %0d", f_guard);',
                        f'        if (f_guard >= {QUIESCE_LIMIT}) $fatal(1, "cache flush timed out");\n'
                        '        $display("FLUSH %0d", f_guard);')
        tb = tb.replace('        $display("QUIESCE %0d", q_guard);',
                        f'        if (q_guard >= {QUIESCE_LIMIT}) $fatal(1, "memory quiescence timed out");\n'
                        '        $display("QUIESCE %0d", q_guard);')
    else:
        tb = tie_off_resets(rtl, tb)
    tb = tb.replace("    integer stack_top;", "    integer stack_top;\n    integer stack_low;")
    tb = tb.replace('        // Two bounds in their own units',
                    '        if (!$value$plusargs("stack_low=%d", stack_low)) $fatal(1, "missing stack bound");\n'
                    '        // Two bounds in their own units', 1)
    tb = tb.replace("            cycle = cycle + 1;",
                    '            if (dut.stack_top_rd < stack_low || dut.stack_top_rd > stack_base)\n'
                    '                $fatal(1, "token stack outside reserved region");\n'
                    "            cycle = cycle + 1;", 1)
    tb = tb.replace('        $finish;\n', '        $display("SIM_COMPLETE");\n        $finish;\n')
    (output / "tb.v").write_text(tb)
    if variant == "production":
        command = ["verilator", "--binary", "--timing", "-Wno-fatal", "-Wno-WIDTH",
                   "-Wno-UNOPTFLAT", "-Wno-LATCH", "-Wno-BLKANDNBLK", "-Wno-MULTIDRIVEN",
                   "--public-flat-rw", "--top-module", "squall_v2_tb", "-j", "1", "-o", "sim",
                   "-f", str(root / "vendor/hpdcache_sources.f"), str(output / "processor.v"),
                   str(output / "tb.v"), "--Mdir", str(output / "obj")]
        compile_command(command, output / "compile.log", root / "vendor")
        runner = [str(output / "obj/sim")]
    else:
        runner = ["vvp", str(output / "sim.vvp")]
        command = ["iverilog", "-g2012", "-s", "squall_v2_tb", "-o", str(output / "sim.vvp"),
                   str(output / "processor.v"), str(output / "tb.v")]
        compile_command(command, output / "compile.log")
    (output / "simulator.json").write_text(json.dumps({
        "runner": runner, "variant": variant, "pipeline_length": depth,
        "memory_latency": latency if variant == "production" else 0,
        "compile_command": command,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
