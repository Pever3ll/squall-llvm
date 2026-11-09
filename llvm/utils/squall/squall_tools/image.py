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
# The Isacomp squall-v2-rebase commit the tools are pinned to (see rtl.py);
# the compiler records the commit it targets in each image's manifest.
ISA_COMMIT = "a21ccd67752c7b8bd9523818aa77b972f13ddb16"


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


def image_relocations(source, readobj=None, objcopy=None):
    """Relocations against the image section. There is no Squall linker, so
    nothing would apply them: the field of an unresolved reference is zero,
    which as a destination makes an instruction send to itself."""
    found = readobj or shutil.which("llvm-readobj")
    if not found and objcopy:
        beside = Path(objcopy).with_name("llvm-readobj")
        found = str(beside) if beside.exists() else None
    if not found:
        raise RuntimeError("put the built llvm-readobj on PATH: the loader checks the image for relocations")
    output = subprocess.run([found, "-r", str(source)], capture_output=True, text=True,
                            check=True, timeout=60).stdout
    section, result = None, []
    for line in output.splitlines():
        if match := re.match(r"\s*Section \(\d+\) (\S+) \{", line):
            section = match[1]
        elif section in (".rela.squall.image", ".rel.squall.image") and "R_SQUALL" in line:
            result.append(line.strip())
    return result


def load_image(source, directory, arguments=(), *, objcopy=None, nm=None, readobj=None,
               memory_bytes=None):
    """Extract the image and write the entry arguments. With memory_bytes,
    the size of the memory the image will be loaded into, refuse an image
    built for a different size: the frame arena, the token stack and the
    stack pointers' reset values all depend on it."""
    source, directory = Path(source).resolve(), Path(directory).resolve()
    if relocations := image_relocations(source, readobj, objcopy):
        raise ValueError(f"object has {len(relocations)} unresolved reference(s) into the image "
                         f"(first: {relocations[0]}); Squall objects are not linked")
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
    isa = manifest.get("isa", {}).get("commit")
    if isa != ISA_COMMIT:
        raise ValueError(f"image targets Isacomp {isa}; these tools are pinned to {ISA_COMMIT}")
    if memory_bytes is not None and memory_bytes != manifest["memory_bytes"]:
        raise ValueError(f"image is built for {manifest['memory_bytes']} bytes of memory, not "
                         f"{memory_bytes}; recompile with -squall-memory-bytes={memory_bytes}")
    # The layout the bring-up contract (BRINGUP.md) relies on. A mismatch
    # means the compiler and this loader disagree, not a bad program.
    reset = manifest["reset"]
    start = int.from_bytes(data[reset["stack_base"]:reset["stack_base"] + 8], "little")
    fill = int(manifest["frame_arena_fill"], 16)
    arena = range(manifest["root_frame_word"] + 1, manifest["frame_arena_end_word"])
    if (reset["stack_base"] != len(data) - 16 or reset["stack_top"] != reset["stack_base"] - 16
            or start & (1 << 29) - 1 != manifest["start_word"]
            or start >> 32 != manifest["root_frame_word"]
            or any(data[w * 8:w * 8 + 8] != fill.to_bytes(8, "little") for w in arena)):
        raise ValueError("image layout does not match its manifest")
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
    parser.add_argument("--readobj")
    parser.add_argument("--memory-bytes", type=lambda v: int(v, 0),
                        help="size of the target's memory; refuse an image built for another size")
    args = parser.parse_args(argv)
    try:
        load_image(args.input, args.output, args.arg, objcopy=args.objcopy, nm=args.nm,
                   readobj=args.readobj, memory_bytes=args.memory_bytes)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"squall-image: {exc}\n")
    return 0
