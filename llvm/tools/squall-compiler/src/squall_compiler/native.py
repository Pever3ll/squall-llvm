"""Unpack native LLVM Squall ELF images; no instruction selection or encoding."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def unpack_image(source, directory, *, objcopy=None):
    source, directory = Path(source).resolve(), Path(directory).resolve()
    objcopy = objcopy or shutil.which('llvm-objcopy')
    if not objcopy:
        raise RuntimeError('put the built llvm-objcopy on PATH or pass --objcopy')
    with tempfile.TemporaryDirectory(prefix='squall-elf-') as scratch:
        scratch = Path(scratch)
        binary, config = scratch / 'memory.bin', scratch / 'manifest.json'
        command = [str(objcopy), f'--dump-section=.squall.image={binary}',
                   f'--dump-section=.squall.config={config}', str(source), str(scratch / 'copy.o')]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f'cannot unpack native Squall object: {result.stderr.strip()}')
        manifest, data = json.loads(config.read_text()), binary.read_bytes()
        if (manifest.get('format') != 'squall-v2-image-1'
                or manifest.get('abi') != 'squall-cell-memory-2'
                or len(data) != manifest.get('memory_bytes') or len(data) % 8):
            raise ValueError('native image size or ABI does not match its manifest')
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'memory.bin').write_bytes(data)
        (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        (directory / 'symbols.json').write_text(json.dumps(manifest.get('symbols', {}), indent=2, sort_keys=True) + '\n')
        words = [(index // 8, int.from_bytes(data[index:index+8], 'little'))
                 for index in range(0, len(data), 8)]
        # Explicit zero initialization keeps repeated launches independent.
        (directory / 'memory.hex').write_text(''.join(f'@{at:x} {value:016x}\n' for at, value in words))
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('--objcopy')
    args = parser.parse_args(argv)
    try:
        unpack_image(args.input, args.output, objcopy=args.objcopy)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f'squall-image: {exc}\n')
    return 0
