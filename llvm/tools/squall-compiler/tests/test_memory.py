from pathlib import Path
import os
import ctypes
import shutil
import subprocess
import pytest
from squall_compiler.compiler import compile_ir, CompileError
from squall_compiler.reference import run_image
from squall_compiler.assembler import assemble


def execute(tmp_path, ir, arguments=(), expected=0, status=1):
    image = compile_ir(ir, arguments=arguments, stack_tokens=2)
    assert assemble(image.assembly.text())[0] == assemble(image.assembly)[0]
    image.write(tmp_path)
    result = run_image(tmp_path, trace=True)
    assert result['quiescent'] and result['status'] == status, result
    if status == 1:
        assert result['result'] == expected
    return image, result


@pytest.mark.parametrize('index,expected', [(0, 7), (1, 8), (2, -2), (3, 16)])
def test_array_index_load_store(tmp_path, index, expected):
    image, _ = execute(tmp_path, '''@a = global [4 x i32] [i32 2, i32 3, i32 -7, i32 11]
    define i32 @main(i32 %i) {
      %p = getelementptr [4 x i32], ptr @a, i32 0, i32 %i
      %v = load i32, ptr %p, align 4
      %r = add i32 %v, 5
      store i32 %r, ptr %p, align 4
      %w = load i32, ptr %p, align 4
      ret i32 %w
    }''', [index], expected)
    assert image.manifest['abi'] == 'squall-cell-memory-2'
    assert image.manifest['memory_model']['pointer_to_word_shift'] == 2


def test_pointer_calls_returns_alias_and_negative_gep(tmp_path):
    execute(tmp_path, '''define ptr @previous(ptr %p) {
      %q = getelementptr i32, ptr %p, i32 -1
      ret ptr %q
    }
    define void @update(ptr %p) { store i32 -91, ptr %p, align 4
      ret void }
    define i32 @main() {
      %array = alloca i32, i32 4
      %end = getelementptr i32, ptr %array, i32 4
      %last = call ptr @previous(ptr %end)
      %holder = alloca ptr
      store ptr %last, ptr %holder, align 4
      %alias = load ptr, ptr %holder, align 4
      call void @update(ptr %alias)
      %result = load i32, ptr %last, align 4
      ret i32 %result
    }''', expected=-91)


@pytest.mark.parametrize('choice,expected', [(0, 17), (1, 31)])
def test_pointer_phi_and_select(tmp_path, choice, expected):
    execute(tmp_path, '''@a = global i32 17
    @b = global i32 31
    define i32 @main(i1 %c) {
      br i1 %c, label %yes, label %no
    yes: br label %end
    no: br label %end
    end:
      %p = phi ptr [@b, %yes], [@a, %no]
      %q = select i1 %c, ptr %p, ptr @a
      %v = load i32, ptr %q, align 4
      ret i32 %v
    }''', [choice], expected)


def test_null_load_traps_before_access(tmp_path):
    execute(tmp_path, 'define i32 @main() { %v = load i32, ptr null, align 4\n ret i32 %v }', status=3)


def test_nested_zero_initialized_global_and_local(tmp_path):
    execute(tmp_path, '''@a = global [2 x [3 x i32]] zeroinitializer
    define i32 @main() {
      %p = getelementptr [2 x [3 x i32]], ptr @a, i32 0, i32 1, i32 2
      %old = load i32, ptr %p, align 4
      store i32 42, ptr %p, align 4
      %new = load i32, ptr %p, align 4
      %result = add i32 %old, %new
      ret i32 %result
    }''', expected=42)


def test_memset_and_memcpy_preserve_cell_values(tmp_path):
    execute(tmp_path, '''declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)
    declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
    define i32 @main() {
      %a = alloca [4 x i32]
      %b = alloca [4 x i32]
      call void @llvm.memset.p0.i32(ptr align 4 %a, i8 -1, i32 16, i1 false)
      call void @llvm.memcpy.p0.p0.i32(ptr align 4 %b, ptr align 4 %a, i32 16, i1 false)
      %last = getelementptr [4 x i32], ptr %b, i32 0, i32 3
      %v = load i32, ptr %last, align 4
      ret i32 %v
    }''', expected=-1)


@pytest.mark.parametrize('ir,error', [
    ('define i32 @main() { %a = alloca i32\n%v = load i32, ptr %a, align 1\nret i32 %v }', '4-byte alignment'),
    ('@a = global [100000000000 x i32] zeroinitializer\ndefine i32 @main() { ret i32 0 }', 'available memory'),
    ('@p = global ptr @p\ndefine i32 @main() { ret i32 0 }', 'null initialization'),
    ('define i32 @main(ptr %p) { ret i32 0 }', 'pointer entry arguments'),
])
def test_unsupported_memory_is_diagnostic(ir, error):
    with pytest.raises(CompileError, match=error):
        compile_ir(ir, arguments=[0] if 'main(ptr' in ir else [])


SQUALL_CLANG = os.environ.get('SQUALL_CLANG')
@pytest.mark.skipif(not shutil.which('clang'), reason='Clang is needed for native differential execution')
def test_c_memory_benchmarks_match_independent_native_execution(tmp_path):
    source = Path(__file__).resolve().parents[1] / 'examples/memory_kernels.c'
    ir, library = tmp_path / 'kernels.ll', tmp_path / 'host.so'
    frontend = SQUALL_CLANG or shutil.which('clang')
    target = 'squallv2-unknown-none' if SQUALL_CLANG else 'i386-unknown-none'
    subprocess.run([frontend, '--target=' + target, '-O0', '-S', '-emit-llvm', str(source), '-o', str(ir)], check=True)
    subprocess.run(['clang', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
    host = ctypes.CDLL(str(library))
    for entry, arguments in [('dot', [0]), ('dot', [8]), ('reduction', [8]), ('local_array', [3]), ('matrix_product', []), ('initialized_local', [2])]:
        image = compile_ir(ir.read_text(), entry=entry, arguments=arguments, stack_tokens=2)
        output = tmp_path / entry / str(arguments)
        image.write(output)
        result = run_image(output)
        assert result['quiescent'] and result['status'] == 1
        assert result['result'] == getattr(host, entry)(*arguments)


def test_allocation_alignment_holds_in_every_activation():
    image = compile_ir('''@g = global [3 x i32] zeroinitializer, align 64
    @h = global i32 0, align 32
    define i32 @main() {
      %first = alloca i32
      %aligned = alloca [4 x i32], align 128
      ret i32 0
    }''', memory_bytes=512 * 1024 + 16)
    manifest = image.manifest
    assert (4 * manifest['globals']['g']) % 64 == 0
    assert (4 * manifest['globals']['h']) % 32 == 0
    offset = manifest['functions']['main']['alloca_slots']['aligned']
    for activation in range(manifest['max_frames']):
        pointer = 4 * (manifest['root_frame_word'] + activation * manifest['frame_stride_words'] + offset)
        assert pointer % 128 == 0


def test_repeated_alloca_requires_dynamic_storage():
    with pytest.raises(CompileError, match='outside the entry block'):
        compile_ir('''define i32 @main() {
          br label %body
        body: %p = alloca i32
          ret i32 0
        }''')


@pytest.mark.parametrize('offset', [-16, 65536, 536870911])
def test_memory_range_guard_rejects_before_rebasing(tmp_path, offset):
    execute(tmp_path, f'''@value = global i32 42
      define i32 @main() {{
        %p = getelementptr i32, ptr @value, i32 {offset}
        %result = load i32, ptr %p, align 4
        ret i32 %result
      }}''', status=3)
