import ctypes
import os
import shutil
import subprocess
import pytest
from squall_compiler.compiler import compile_ir
from squall_compiler.reference import run_image
from squall_compiler.random_programs import generate


@pytest.mark.skipif(not shutil.which('clang'), reason='Clang is needed for independent native execution')
@pytest.mark.parametrize('seed', [0, 1, 7, 42])
def test_seeded_memory_control_and_unsigned_arithmetic(tmp_path, seed):
    source, ir, library = (tmp_path / name for name in ('program.c', 'program.ll', 'host.so'))
    source.write_text(generate(seed))
    frontend = os.environ.get('SQUALL_CLANG') or shutil.which('clang')
    target = 'squallv2-unknown-none' if os.environ.get('SQUALL_CLANG') else 'i386-unknown-none'
    subprocess.run([frontend, '--target=' + target, '-O0', '-S', '-emit-llvm', str(source), '-o', str(ir)], check=True)
    subprocess.run(['clang', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
    oracle = ctypes.CDLL(str(library)).kernel
    oracle.argtypes, oracle.restype = [ctypes.c_uint32, ctypes.c_int32], ctypes.c_uint32
    for value, count in [(0, 0), (0xffffffff, 8), (seed, 5)]:
        image = tmp_path / f'{value}-{count}'
        compile_ir(ir.read_text(), entry='kernel', arguments=[value, count], stack_tokens=2).write(image)
        result = run_image(image)
        assert result['quiescent'] and result['status'] == 1
        assert (result['result'] & 0xffffffff) == oracle(value, count)
