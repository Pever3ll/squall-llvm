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


def harden(tb):
    """Fail loudly on a stack overrun, and mark a run that reached the end."""
    tb = tb.replace("    integer stack_top;", "    integer stack_top;\n    integer stack_low;", 1)
    tb = tb.replace('        if (!$value$plusargs("max_tokens=%d", max_tokens))',
                    '        if (!$value$plusargs("stack_low=%d", stack_low)) $fatal(1, "missing stack bound");\n'
                    '        if (!$value$plusargs("max_tokens=%d", max_tokens))', 1)
    tb = tb.replace("            cycle = cycle + 1;",
                    '            if (dut.stack_top_rd < stack_low || dut.stack_top_rd > stack_base)\n'
                    '                $fatal(1, "token stack outside reserved region");\n'
                    "            cycle = cycle + 1;", 1)
    tb = tb.replace('        $finish;\n    end\nendmodule', '        $display("SIM_COMPLETE");\n        $finish;\n    end\nendmodule', 1)
    for marker in ("stack_low", "token stack outside", "SIM_COMPLETE"):
        if marker not in tb:
            raise RuntimeError(f"upstream testbench changed; cannot add {marker}")
    return tb


def main():
    root, output, variant, latency = sys.argv[1:]
    root, output, latency = Path(root).resolve(), Path(output).resolve(), int(latency)
    sys.path[:0] = [str(root), str(root / "test"), str(root / "isa/squall/generators"),
                    str(root / "isa/squall/tools")]
    output.mkdir(parents=True, exist_ok=True)
    import squall_v2_fixtures as F
    from create_squall_pipeline import pipelined
    from squall_build import build, verilog

    with contextlib.redirect_stdout(io.StringIO()) as generation:
        if variant == "production":
            # The actual production generator, with its own pass order and
            # cache geometries; not a lookalike.
            from create_squall_prod import main as production_main
            if production_main(["-o", str(output / "processor.v")]) not in (None, 0):
                raise RuntimeError("production generator failed")
            rtl = (output / "processor.v").read_text()
            depth = None
        else:
            graph = build(quiet=True) if variant == "baseline" else pipelined("full", "stall", quiet=True)
            rtl = verilog(graph, quiet=True)
            depth = (graph.get_max_pipe_time() or 0) + 1
    (output / "generate.log").write_text(generation.getvalue())
    (output / "processor.v").write_text(rtl)
    if variant == "production":
        # As create_squall_prod.check() prepares its bench.
        tb = (root / "isa/squall/squall_prod_tb.v").read_text()
        tb = F.wire_cache_flush(rtl, F.tie_off_resets(rtl, F.retarget_token_read(rtl, tb)))
        if "PIPE_Decode_stall_ctrl" in rtl:
            tb = tb.replace("`define TB_STALL_EXPR 1'b0",
                            "`define TB_STALL_EXPR dut.PIPE_Decode_stall_ctrl")
        depth = 6
    else:
        tb = retarget((root / "isa/squall/squall_v2_tb.v").read_text(), rtl)
        tb = F.tie_off_resets(rtl, tb)
    tb = harden(tb)
    (output / "tb.v").write_text(tb)
    if variant == "production":
        command = ["verilator", "--binary", "--timing", "-Wno-fatal", "-Wno-WIDTH",
                   "-Wno-UNOPTFLAT", "-Wno-LATCH", "-Wno-BLKANDNBLK", "-Wno-MULTIDRIVEN",
                   "--public-flat-rw", "--top-module", "squall_v2_tb", "-j", "1", "-o", "sim"]
        if "hpdcache" in rtl:
            command += ["-f", str(root / "vendor/hpdcache_sources.f")]
        for required in re.findall(r"^// isacomp-requires: (\S+)$", rtl, re.M):
            if "hpdcache" not in required:
                command += ["-F", str(root / required)]
        command += [str(output / "processor.v"), str(output / "tb.v"), "--Mdir", str(output / "obj")]
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
