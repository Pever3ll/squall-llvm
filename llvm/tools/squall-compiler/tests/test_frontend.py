import ctypes
from pathlib import Path
import shutil
import subprocess

import pytest

from squall_compiler.compiler import compile_ir
from squall_compiler.reference import run_image


@pytest.mark.skipif(not shutil.which("clang"), reason="Clang is needed for C frontend integration")
def test_clang_c_to_llvm_to_squall_matches_host_execution(tmp_path):
    source = Path(__file__).resolve().parents[1] / "examples/kernel.c"
    llvm_file = tmp_path / "kernel.ll"
    library = tmp_path / "kernel.so"
    subprocess.run(["clang", "-O0", "-S", "-emit-llvm", str(source), "-o", str(llvm_file)], check=True)
    subprocess.run(["clang", "-O0", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
    host = ctypes.CDLL(str(library)).kernel
    host.argtypes, host.restype = [ctypes.c_int32], ctypes.c_int32
    for value in [0, 1, 2, 7, 12]:
        output = tmp_path / f"run-{value}"
        compile_ir(llvm_file.read_text(), entry="kernel", arguments=[value], stack_tokens=2).write(output)
        result = run_image(output)
        assert result["quiescent"] and result["status"] == 1, result
        assert result["result"] == host(value)
