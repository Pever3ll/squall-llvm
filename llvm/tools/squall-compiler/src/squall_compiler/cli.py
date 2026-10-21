"""Command-line interfaces for the standalone Squall compiler and assembler."""
import argparse
import json
from pathlib import Path
import sys

from .assembler import AssemblyError, assemble
from .compiler import CompileError, compile_ir


def compile_main(argv=None):
    parser = argparse.ArgumentParser(description="Compile verified LLVM IR to a Squall v2 loadable image")
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--entry", default="main")
    parser.add_argument("--arg", action="append", default=[], type=lambda value: int(value, 0))
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--memory-bytes", type=lambda value: int(value, 0), default=512 * 1024)
    parser.add_argument("--stack-tokens", type=int, default=1024)
    args = parser.parse_args(argv)
    try:
        image = compile_ir(args.input.read_text(), args.entry, args.arg, memory_bytes=args.memory_bytes,
                           frames=args.frames, stack_tokens=args.stack_tokens)
        image.write(args.output)
        print(f"wrote {args.output}: {len(image.assembly.records)} code/data words, ABI {image.manifest['abi']}")
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"squall-compile: {exc}", file=sys.stderr)
        return 1


def asm_main(argv=None):
    parser = argparse.ArgumentParser(description="Assemble labelled Squall records to a word-indexed .hex file")
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--origin", type=lambda value: int(value, 0), default=256,
                        help="origin in 64-bit words, not bytes")
    args = parser.parse_args(argv)
    try:
        words, labels = assemble(args.input.read_text(), args.origin)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("".join(f"@{at:x} {value:016x}\n" for at, value in sorted(words.items())))
        args.output.with_suffix(".symbols.json").write_text(json.dumps(labels, indent=2) + "\n")
        return 0
    except (OSError, AssemblyError) as exc:
        print(f"squall-asm: {exc}", file=sys.stderr)
        return 1
