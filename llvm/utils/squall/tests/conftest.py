"""Shared helpers: every test compiles with the built LLVM tools (llc, clang,
llvm-mc) and executes on the pinned Zig reference interpreter or the RTL."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from squall_tools.image import load_image
from squall_tools.reference import run_image

BIN = os.environ.get("SQUALL_LLVM_BIN")
EXAMPLES = Path(__file__).resolve().parents[1] / "examples"

requires_llvm = pytest.mark.skipif(not BIN, reason="set SQUALL_LLVM_BIN to the built LLVM bin directory")


def tool(name):
    return str(Path(BIN) / name)


def run_tool(name, *arguments, check=True, input=None):
    return subprocess.run([tool(name), *map(str, arguments)], capture_output=True, text=True,
                          check=check, timeout=120, input=input)


def i32(value):
    value &= 0xffffffff
    return value - (1 << 32) if value >= 1 << 31 else value


def load(obj, directory, arguments=()):
    return load_image(obj, directory, arguments, objcopy=tool("llvm-objcopy"), nm=tool("llvm-nm"))


def compile_ir(directory, ir, arguments=(), *, entry="main", opt="-O0", options=()):
    """llc -> ELF object -> loaded image directory."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    source, obj = directory / "input.ll", directory / "image.o"
    source.write_text(ir)
    run_tool("llc", "-mtriple=squallv2", "-filetype=obj", "-verify-machineinstrs", opt,
             f"-squall-entry={entry}", *options, source, "-o", obj)
    load(obj, directory / "image", arguments)
    return directory / "image"


def execute(directory, ir, arguments=(), expected=None, *, status=1, **options):
    image = compile_ir(directory, ir, arguments, **options)
    result = run_image(image)
    assert result["quiescent"], result
    assert result["status"] == status, result
    if expected is not None:
        assert result["result"] == expected, result
    return result


def clang():
    return os.environ.get("SQUALL_CLANG") or (tool("clang") if BIN and Path(tool("clang")).exists() else None)


def host_clang():
    return shutil.which("clang") or shutil.which("cc")


@pytest.fixture(params=["-O0", "-O2"])
def opt(request):
    return request.param
