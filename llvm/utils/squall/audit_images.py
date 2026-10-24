#!/usr/bin/env python3
"""Regenerate saved regression images and compare them with the final compiler."""
import argparse
import hashlib
import json
from pathlib import Path

from squall_compiler.assembler import assemble
from squall_compiler.compiler import compile_ir


def audit(directory):
    total = 0
    for image_dir in sorted((directory / 'images').iterdir()):
        if not image_dir.is_dir():
            continue
        manifest = json.loads((image_dir / 'manifest.json').read_text())
        image = compile_ir((image_dir / 'input.ll').read_text(), entry=manifest['entry'],
                           arguments=manifest['arguments'], memory_bytes=manifest['memory_bytes'],
                           frames=manifest['max_frames'], stack_tokens=manifest['stack_token_capacity'])
        code, symbols = assemble((image_dir / 'program.sqa').read_text())
        if code != assemble(image.assembly)[0] or symbols != image.symbols:
            raise RuntimeError(f'{image_dir.name}: assembly differs from regenerated source')
        binary = bytearray(manifest['memory_bytes'])
        for address, value in image.words.items():
            binary[address * 8:address * 8 + 8] = value.to_bytes(8, 'little')
        if binary != (image_dir / 'memory.bin').read_bytes():
            raise RuntimeError(f'{image_dir.name}: binary differs from regenerated source')
        if manifest != image.manifest:
            raise RuntimeError(f'{image_dir.name}: manifest differs from regenerated source')
        total += 1
    if not total:
        raise RuntimeError('no saved regression images found')
    source = Path(__file__).resolve().parents[2] / 'tools/squall-compiler'
    hashes = {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted((source / 'src/squall_compiler').glob('*'))
              if path.suffix in ('.py', '.zig')}
    report = {'images': total, 'byte_identical': True, 'compiler_source_sha256': hashes}
    (directory / 'final-source-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{total} saved images, assembly listings, and manifests match final compiler source')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('regression', type=Path)
    audit(parser.parse_args().regression.resolve())
