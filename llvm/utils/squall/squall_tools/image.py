"""Load a Squall ELF object produced by llc/clang into a runnable memory image.

This is the loader step only: it extracts the already encoded `.squall.image`
section, writes the entry arguments into their argument slots, and records the
symbol table. It performs no code generation or instruction encoding.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

FORMAT = "squall-v2-image-2"
ABI = "squall-frame-slots-2"


def constant_slot(value):
    """A matching slot holding a constant: presence bits 0b10 and a 62-bit
    payload, which a slot read zero-extends."""
    if not 0 <= value < 1 << 62:
        raise ValueError(f"slot payload {value} does not fit unsigned 62 bits")
    return 2 << 62 | value


def canonical(value, kind, extension="none"):
    if kind == "i1":
        if value not in (0, 1):
            raise ValueError(f"i1 argument must be 0 or 1, got {value}")
        bits = 1  # A signext true is -1.
    elif kind == "ptr":
        if not 0 <= value < 1 << 32:
            raise ValueError(f"pointer argument {value} does not fit 32 bits")
        bits = 32
    else:
        bits = int(kind[1:])
        if not -(1 << bits - 1) <= value < 1 << bits:
            raise ValueError(f"{kind} argument {value} does not fit {bits} bits")
    # A narrow argument converts as in C (keep its low bits), and the entry
    # function relies on the extension its signext/zeroext attribute names.
    value &= (1 << bits) - 1
    if extension == "sext" and value >= 1 << bits - 1:
        value -= 1 << bits
    # Registers hold 32-bit values zero-extended to the 64-bit slot, pointers
    # as well as integers: a slot read zero-extends.
    return value & 0xffffffff


def tool(name, explicit=None):
    found = explicit or shutil.which(name)
    if not found:
        raise RuntimeError(f"put the built {name} on PATH or pass its path")
    return str(found)


def image_symbols(source, nm=None):
    """Word addresses of the symbols defined in the image section."""
    output = subprocess.run([tool("llvm-nm", nm), "--defined-only", "--no-sort",
                             str(source)], capture_output=True, text=True,
                            check=True, timeout=60).stdout
    symbols = {}
    # "value type name": the name is the rest of the line, and may itself
    # contain spaces or tabs (LLVM allows any character but a newline). Weak
    # definitions are W (functions) or V (objects); --defined-only already
    # leaves out undefined weak references.
    for line in output.split("\n"):
        match = re.fullmatch(r"([0-9a-fA-F]+) ([tTdDbBWV]) (.+)", line)
        if match and int(match[1], 16) % 8 == 0:
            symbols[match[3]] = int(match[1], 16) // 8
    return symbols


def load_image(source, directory, arguments=(), *, objcopy=None, nm=None):
    source, directory = Path(source).resolve(), Path(directory).resolve()
    with tempfile.TemporaryDirectory(prefix="squall-elf-") as scratch:
        scratch = Path(scratch)
        binary, config = scratch / "memory.bin", scratch / "manifest.json"
        result = subprocess.run(
            [tool("llvm-objcopy", objcopy), f"--dump-section=.squall.image={binary}",
             f"--dump-section=.squall.config={config}", str(source), str(scratch / "copy.o")],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f"cannot read Squall object: {result.stderr.strip()}")
        manifest = json.loads(config.read_text())
        data = bytearray(binary.read_bytes())
    if (manifest.get("format") != FORMAT or manifest.get("abi") != ABI
            or len(data) != manifest.get("memory_bytes")):
        raise ValueError("object is not a complete Squall image for this ABI")
    arguments = list(arguments)
    kinds = manifest["argument_types"]
    if len(arguments) != len(kinds):
        raise ValueError(f"entry '{manifest['entry']}' takes {len(kinds)} arguments, "
                         f"got {len(arguments)}")
    extensions = manifest.get("argument_extensions", ["none"] * len(kinds))
    registers = [canonical(v, k, e) for v, k, e in zip(arguments, kinds, extensions)]
    for word, value in zip(manifest["argument_words"], registers):
        data[word * 8:word * 8 + 8] = constant_slot(value).to_bytes(8, "little")
    symbols = image_symbols(source, nm)
    entry = manifest["entry"]
    if entry not in symbols:
        raise ValueError(f"entry '{entry}' is not among the image's symbols")
    manifest.update(arguments=registers,
                    symbols=symbols, bootstrap_slots=[],
                    functions={entry: {"entry_word": symbols[entry]}})
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "memory.bin").write_bytes(data)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    words = (int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data), 8))
    # Zero words are written too, so repeated loads into one memory start clean.
    (directory / "memory.hex").write_text("".join(f"@{at:x} {value:016x}\n"
                                                  for at, value in enumerate(words)))
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Squall ELF object from llc or clang")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--arg", action="append", default=[], type=lambda v: int(v, 0),
                        help="entry argument, repeated in order")
    parser.add_argument("--objcopy")
    parser.add_argument("--nm")
    args = parser.parse_args(argv)
    try:
        load_image(args.input, args.output, args.arg, objcopy=args.objcopy, nm=args.nm)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"squall-image: {exc}\n")
    return 0
