"""Exercise actual LLVM binaries, with Python used only as an independent oracle."""
import os
from pathlib import Path
import subprocess
import shutil
import pytest
from squall_compiler.assembler import OPS, Program, assemble
from squall_compiler.native import unpack_image
from squall_compiler.reference import run_image
from squall_compiler.rtl import run_rtl

BIN = os.environ.get('SQUALL_LLVM_BIN')
pytestmark = pytest.mark.skipif(not BIN, reason='set SQUALL_LLVM_BIN to the built native LLVM binaries')


def tool(name, *args, check=True):
    return subprocess.run([str(Path(BIN) / name), *map(str, args)], capture_output=True, text=True, check=check, timeout=60)


def native_image(tmp_path, ir, arguments=(), entry='main', options=()):
    tmp_path.mkdir(parents=True, exist_ok=True)
    source, obj = tmp_path / 'input.ll', tmp_path / 'native.o'
    source.write_text(ir)
    tool('llc', '-mtriple=squallv2-unknown-none', '-filetype=obj', '-squall-entry='+entry,
         *['-squall-arg='+str(a) for a in arguments], *options, source, '-o', obj)
    unpack_image(obj, tmp_path, objcopy=Path(BIN) / 'llvm-objcopy')
    return tmp_path


@pytest.mark.parametrize('op', list(OPS))
def test_mc_encodings_and_disassembly_match_independent_assembler(tmp_path, op):
    program = Program()
    program.add('start', 'monadic', op=op, imm=-2147483648, dest='end.R', second='(1).L')
    program.add('a', 'dyadic', op=op, addr='frame:2047', match='Read', dest='start.L')
    program.add('b', 'dyadic', op=op, addr='code:end', match='Const', dest='end.R')
    program.add('c', 'dyadic', op=op, addr='abs:17', match='Once', dest='(1).L')
    program.add('end', 'write', addr=1)
    source, obj, binary = (tmp_path / x for x in ('program.s', 'program.o', 'text.bin'))
    source.write_text(program.text())
    tool('llvm-mc', '-triple=squallv2', '-filetype=obj', source, '-o', obj)
    tool('llvm-objcopy', '--dump-section=.text='+str(binary), obj, tmp_path / 'copy.o')
    expected, _ = assemble(program)
    assert binary.read_bytes() == b''.join(word.to_bytes(8, 'little') for word in expected.values())
    disassembly = tool('llvm-objdump', '-d', obj).stdout
    assert op.lower() in disassembly
    raw = binary.read_bytes()
    text = subprocess.run([str(Path(BIN)/'llvm-mc'), '-triple=squallv2', '-disassemble'],
                          input=' '.join(f'0x{byte:02x}' for byte in raw),
                          text=True, capture_output=True, check=True, timeout=60).stdout
    decoded = tmp_path/'decoded.s'; decoded.write_text(text)
    tool('llvm-mc','-triple=squallv2','-filetype=obj',decoded,'-o',tmp_path/'decoded.o')
    tool('llvm-objcopy','--dump-section=.text='+str(tmp_path/'decoded.bin'),tmp_path/'decoded.o',tmp_path/'decoded-copy.o')
    assert (tmp_path/'decoded.bin').read_bytes() == raw


@pytest.mark.parametrize('text', [
    'add.r (1).l [2048]', 'add (1).l #2147483648', 'add (524288).l #0',
    'send.r (1).l.r [0]', 'extract.r (1).l (1).l.r [0]',
    'write -1', 'add.x (1).l [0]', '.slot 2305843009213693952',
])
def test_native_assembler_rejects_invalid_fields(tmp_path, text):
    source = tmp_path / 'invalid.s'; source.write_text(text+'\n')
    result = tool('llvm-mc', '-triple=squallv2', '-filetype=obj', source, '-o', tmp_path / 'bad.o', check=False)
    assert result.returncode and 'error:' in result.stderr


def test_send_extract_data_and_halt(tmp_path):
    text = '''start: extract.r remote.r target.l [0]
remote: send.c halt.l (data)
target: add halt.l #-7
data: .slot 35
raw: .word -1
'''
    source=tmp_path/'program.s';source.write_text(text)
    tool('llvm-mc','-triple=squallv2','-filetype=obj',source,'-o',tmp_path/'program.o')
    tool('llvm-objcopy','--dump-section=.text='+str(tmp_path/'text.bin'),tmp_path/'program.o',tmp_path/'copy.o')
    expected,_=assemble(text)
    assert (tmp_path/'text.bin').read_bytes()==b''.join(w.to_bytes(8,'little') for w in expected.values())


@pytest.mark.parametrize('arguments', [(7,35), (-2147483648,-1), (0,0)])
def test_llc_assembly_and_object_execution(tmp_path, arguments):
    ir='define i32 @main(i32 %a, i32 %b) { %r = add i32 %a, %b\n ret i32 %r }'
    native_image(tmp_path,ir,arguments)
    result=run_image(tmp_path)
    assert result['quiescent'] and result['status']==1
    assert result['result']==((sum(arguments)+2**31)%2**32)-2**31
    tool('llc','-mtriple=squallv2','-squall-arg='+str(arguments[0]),'-squall-arg='+str(arguments[1]),tmp_path/'input.ll','-o',tmp_path/'native.s')
    tool('llvm-mc','-triple=squallv2','-filetype=obj',tmp_path/'native.s','-o',tmp_path/'roundtrip.o')
    unpack_image(tmp_path/'roundtrip.o',tmp_path/'roundtrip',objcopy=Path(BIN)/'llvm-objcopy')
    assert (tmp_path/'memory.bin').read_bytes()==(tmp_path/'roundtrip/memory.bin').read_bytes()


@pytest.mark.parametrize('entry,arguments,expected', [('dot',[8],163),('reduction',[8],45),('local_array',[3],1194),('matrix_product',[],155),('initialized_local',[2],7)])
def test_native_memory_calls_and_intrinsics(tmp_path, entry, arguments, expected):
    clang=os.environ.get('SQUALL_CLANG') or shutil.which('clang')
    if not clang: pytest.skip('Clang is needed for C frontend integration')
    target='squallv2' if os.environ.get('SQUALL_CLANG') else 'i386-unknown-none'
    source=Path(__file__).resolve().parents[1]/'examples/memory_kernels.c'
    ir=tmp_path/'kernels.ll'
    subprocess.run([clang,'--target='+target,'-O0','-S','-emit-llvm',source,'-o',ir],check=True)
    native_image(tmp_path,ir.read_text(),arguments,entry)
    result=run_image(tmp_path)
    assert result['quiescent'] and result['status']==1 and result['result']==expected


def test_native_recursive_control_flow(tmp_path):
    ir=(Path(__file__).resolve().parents[1]/'examples/fibonacci.ll').read_text()
    native_image(tmp_path,ir,[8])
    assert run_image(tmp_path)['result']==21


@pytest.mark.skipif(not os.environ.get('SQUALL_ISACOMP_DIR'),reason='pinned RTL checkout is required')
def test_native_llc_image_executes_on_rtl(tmp_path):
    native_image(tmp_path,'define i32 @main(i32 %a) { %r = mul i32 %a, 6\n ret i32 %r }',[7])
    result=run_rtl(tmp_path)
    assert result['result']==42 and result['trace_matches'] and result['memory_matches']


def test_clang_driver_generates_executable_squall_object(tmp_path):
    clang=os.environ.get('SQUALL_CLANG')
    if not clang: pytest.skip('set SQUALL_CLANG for the built Squall frontend')
    source=tmp_path/'main.c'; source.write_text('int main(void) { return 42; }')
    subprocess.run([clang,'--target=squallv2','-O0','-c',source,'-o',tmp_path/'main.o'],check=True)
    unpack_image(tmp_path/'main.o',tmp_path,objcopy=Path(BIN)/'llvm-objcopy')
    assert run_image(tmp_path)['result']==42


@pytest.mark.parametrize('ir,error', [
    ('define i64 @main() { ret i64 0 }', 'supported values'),
    ('define i32 @main() { %a = alloca i32, i128 1267650600228229401496703205376\nret i32 0 }', 'alloca requires'),
    ('define i32 @main() { %a = alloca i32, addrspace(1)\nret i32 0 }', 'nondefault address spaces'),
    ('define i32 @main() { %a = alloca i32\n %v = load i32, ptr %a, align 1\n ret i32 %v }', 'four-byte-aligned'),
    ('declare i32 @external()\ndefine i32 @main() { %v = call i32 @external()\n ret i32 %v }', 'external/indirect'),
    ('define i32 @main() { br label %b\nb: %a = alloca i32\nret i32 0 }', 'alloca outside'),
])
def test_llc_unsupported_input_is_a_diagnostic(tmp_path, ir, error):
    source=tmp_path/'invalid.ll';source.write_text(ir)
    result=tool('llc','-mtriple=squallv2',source,'-o',tmp_path/'bad.s',check=False)
    assert result.returncode>0 and error in result.stderr
    assert 'PLEASE submit' not in result.stderr


def test_native_recursion_depth_and_memory_guards(tmp_path):
    ir=(Path(__file__).resolve().parents[1]/'examples/fibonacci.ll').read_text()
    native_image(tmp_path/'depth',ir,[8],options=['-squall-frames=2'])
    result=run_image(tmp_path/'depth')
    assert result['quiescent'] and result['status']==2
    native_image(tmp_path/'memory','define i32 @main() { %v = load i32, ptr null, align 4\nret i32 %v }')
    result=run_image(tmp_path/'memory')
    assert result['quiescent'] and result['status']==3


def test_native_assembler_origin_and_separated_secondary_alias(tmp_path):
    source=tmp_path/'origin.s'
    text='add.r halt.l r [4]\nwrite 1\n'
    source.write_text('.squall_origin 4096\n'+text)
    tool('llvm-mc','-triple=squallv2','-filetype=obj',source,'-o',tmp_path/'origin.o')
    tool('llvm-objcopy','--dump-section=.text='+str(tmp_path/'text.bin'),tmp_path/'origin.o',tmp_path/'copy.o')
    expected,_=assemble(text,origin=4096)
    assert (tmp_path/'text.bin').read_bytes()==b''.join(w.to_bytes(8,'little') for w in expected.values())
