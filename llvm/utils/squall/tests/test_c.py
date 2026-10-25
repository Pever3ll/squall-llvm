"""C compiled by the built Squall Clang, checked against native host execution."""
import ctypes
import shutil
import subprocess

import pytest

from conftest import EXAMPLES, clang, host_clang, i32, load, requires_llvm
from squall_tools.random_programs import generate
from squall_tools.reference import run_image

pytestmark = [requires_llvm,
              pytest.mark.skipif(not host_clang(), reason="a host C compiler is the oracle")]

CASES = {
    "kernel.c": {"kernel": [(0,), (1,), (7,), (12,)]},
    "rtl_kernels.c": {
        "add": [(7, 35), (-1, 1), (-100, 25)],
        "fanout": [(0,), (-1,), (-2147483648,), (2147483647,)],
        "unsigned_divrem": [(0, 3), (-1, 3), (-2147483648, 7), (-1, -1)],
        "signed_divrem": [(-7, 3), (7, -3), (-2147483648, 31)],
        "shifts": [(-1, 1), (-2147483648, 31), (2147483647, 0)],
        "comparisons": [(0, 0), (-1, 0), (-2147483648, 2147483647)],
        "branch": [(7, -29, 1), (7, -29, 0)],
        "loop": [(0,), (7,), (100,)],
        "memory_calls": [(0,), (10,), (100,)],
        "fibonacci": [(0,), (5,), (8,)],
        "calls": [(3, 4, 10, 4), (7, -3, -5, 2)],
    },
    "memory_kernels.c": {
        "dot": [(0,), (4,), (8,)],
        "reduction": [(0,), (8,)],
        "local_array": [(0,), (3,)],
        "matrix_product": [()],
        "initialized_local": [(0,), (3,)],
    },
    "bytes.c": {
        "string_length": [(0,), (3,), (9,)],
        "reverse": [(0,), (5,)],
        "concatenate": [(0,), (1,), (2,), (3,)],
        "sort_letters": [(0,), (6,), (13,)],
        "signed_bytes": [(0,), (100,), (-77,)],
        "shorts": [(1,), (-3,)],
        "records_by_value": [(0,), (4,)],
        "packed_fields": [(0,), (12345,)],
        "adjacent_bytes": [(3,), (-91,)],
        "variable_copies": [(0,), (13,), (29,), (38,)],
        "wide_division": [(12345, 67), (-99999, 13), (2147483647, -1), (-2147483648, 0)],
    },
    "callbacks.c": {
        "dispatch": [(0, 7, 5), (1, 7, 5), (2, -7, 5), (3, 2, 9), (7, 4, 4)],
        "folds": [(0,), (3,), (-5,)],
        "handler_table": [(0,), (4,), (-3,)],
        "sort_with": [(1,), (2,), (13,)],
        "recursive_pointer": [(0,), (17,), (100,)],
        "returned_pointer": [(0,), (1,), (2,), (5,)],
    },
    "programs.c": {
        "points": [(0,), (3,), (-7,)],
        "classify": [(0,), (2,), (7,), (-3,), (100,)],
        "sort": [(1,), (12345,)],
        "gcd": [(1071, 462), (-48, 18), (17, 0)],
        "collatz": [(1,), (27,)],
        "histogram": [(0,), (40,)],
    },
}


def host_library(tmp_path, source):
    library = tmp_path / "host.so"
    subprocess.run([host_clang(), "-O0", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
    return ctypes.CDLL(str(library))


def fresh_function(tmp_path, library, entry, index):
    """A host function from a private copy of the library: some examples
    modify globals, and every Squall image starts from the initial values."""
    copy = tmp_path / f"host-{entry}-{index}.so"
    shutil.copy(library, copy)
    return getattr(ctypes.CDLL(str(copy)), entry)


def compile_c(tmp_path, source, entry, level):
    obj = tmp_path / f"{entry}{level}.o"
    subprocess.run([clang(), "--target=squallv2", level, "-c", "-mllvm", f"-squall-entry={entry}",
                    str(source), "-o", str(obj)], check=True, capture_output=True, text=True)
    return obj


@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2"])
@pytest.mark.parametrize("source", list(CASES))
def test_examples_match_native_execution(tmp_path, source, level):
    if not clang():
        pytest.skip("the built Squall Clang is required")
    path = EXAMPLES / source
    host_library(tmp_path, path)
    for entry, inputs in CASES[source].items():
        obj = compile_c(tmp_path, path, entry, level)
        for index, arguments in enumerate(inputs):
            function = fresh_function(tmp_path, tmp_path / "host.so", entry, index)
            function.argtypes = [ctypes.c_int32] * len(arguments)
            function.restype = ctypes.c_uint32
            image = tmp_path / f"{entry}{level}-{index}"
            load(obj, image, arguments)
            result = run_image(image)
            assert result["quiescent"] and result["status"] == 1, (entry, arguments, result)
            assert result["result"] == i32(function(*arguments)), (entry, arguments, result)


@pytest.mark.parametrize("seed", [0, 1, 7, 42])
@pytest.mark.parametrize("level", ["-O0", "-O2"])
def test_seeded_programs(tmp_path, seed, level):
    if not clang():
        pytest.skip("the built Squall Clang is required")
    source = tmp_path / "program.c"
    source.write_text(generate(seed))
    oracle = host_library(tmp_path, source).kernel
    oracle.argtypes, oracle.restype = [ctypes.c_uint32, ctypes.c_int32], ctypes.c_uint32
    obj = compile_c(tmp_path, source, "kernel", level)
    for value, count in [(0, 0), (0xffffffff, 8), (seed, 5)]:
        image = tmp_path / f"{value}-{count}"
        load(obj, image, [value, count])
        result = run_image(image)
        assert result["quiescent"] and result["status"] == 1
        assert result["result"] == i32(oracle(value, count))
