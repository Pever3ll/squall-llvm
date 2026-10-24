#!/usr/bin/env python3
"""Rebuild saved native LLVM images and bind RTL evidence to current binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from squall_compiler.native import unpack_image


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('regression', type=Path)
    parser.add_argument('--native-bin', required=True, type=Path)
    parser.add_argument('--compare-python', action='store_true', help='Also require byte identity with the independent baseline compiler')
    args = parser.parse_args()
    root, binary = args.regression.resolve(), args.native_bin.resolve()
    checks, independent = [], []
    for image in sorted((root / 'images').iterdir()):
        manifest = json.loads((image / 'manifest.json').read_text())
        with tempfile.TemporaryDirectory(prefix='squall-native-audit-') as scratch:
            scratch = Path(scratch)
            command = [str(binary / 'llc'), '-mtriple=squallv2', '-filetype=obj',
                       '-squall-entry='+manifest['entry'],
                       *['-squall-arg='+str(value) for value in manifest['arguments']],
                       '-squall-memory-bytes='+str(manifest['memory_bytes']),
                       '-squall-frames='+str(manifest['max_frames']),
                       '-squall-stack-tokens='+str(manifest['stack_token_capacity']),
                       str(image / 'input.ll'), '-o', str(scratch / 'native.o')]
            subprocess.run(command, check=True, timeout=60)
            updated = unpack_image(scratch / 'native.o', scratch, objcopy=binary / 'llvm-objcopy')
            if (scratch / 'memory.bin').read_bytes() != (image / 'memory.bin').read_bytes():
                raise RuntimeError(f'final native code changed the tested machine image: {image.name}')
            for field in ('abi', 'entry', 'arguments', 'memory_bytes', 'stack_base', 'stack_top',
                          'root_frame_word', 'frame_stride_words', 'functions', 'globals',
                          'memory_model', 'bootstrap_slots', 'symbols'):
                if manifest[field] != updated[field]:
                    raise RuntimeError(f'final native code changed {field}: {image.name}')
            if args.compare_python:
                from squall_compiler.compiler import compile_ir
                baseline = compile_ir((image / 'input.ll').read_text(), entry=manifest['entry'],
                                      arguments=manifest['arguments'], memory_bytes=manifest['memory_bytes'],
                                      frames=manifest['max_frames'], stack_tokens=manifest['stack_token_capacity'])
                data = bytearray(manifest['memory_bytes'])
                for at, value in baseline.words.items():
                    data[at*8:at*8+8] = value.to_bytes(8, 'little')
                identical = bytes(data) == (image / 'memory.bin').read_bytes()
                independent.append({'case': image.name, 'identical': identical})
                if not identical:
                    raise RuntimeError(f'native/Python machine images differ: {image.name}')
            checks.append({'case': image.name, 'memory_sha256': digest(image / 'memory.bin'),
                           'ir_sha256': digest(image / 'input.ll')})
    # On a shared build, llc loads the target implementation from libLLVM.
    libraries = sorted((binary.parent / 'lib').glob('libLLVM*.dylib')) + sorted((binary.parent / 'lib').glob('libLLVM*.so*'))
    tools = {str(path): digest(path) for path in [binary / 'llc', binary / 'llvm-mc', *libraries] if path.is_file()}
    report = {'images': len(checks), 'matched': len(checks), 'binaries': tools, 'checks': checks}
    (root / 'final-native-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.compare_python:
        (root / 'independent-compiler-comparison.json').write_text(json.dumps(
            {'cases': len(independent), 'byte_identical': len(independent), 'rows': independent}, indent=2) + '\n')
    print(f'{len(checks)}/{len(checks)} native image binaries and ABI layouts match final LLVM binaries')


if __name__ == '__main__':
    main()
