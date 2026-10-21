import os
import pytest
from squall_compiler.compiler import compile_ir
from squall_compiler.rtl import run_rtl


@pytest.mark.parametrize('options,error', [
    ({'stall_seed': -1}, 'stall seed'),
    ({'stall_jitter': 201}, 'jitter'),
    ({'stall_jitter': 1}, 'require production'),
    ({'variant': 'production', 'memory_latency': 0, 'stall_jitter': 1}, 'positive memory latency'),
])
def test_invalid_random_stalls_fail_before_execution(options, error):
    with pytest.raises(RuntimeError, match=error):
        run_rtl('does-not-exist', **options)


@pytest.mark.skipif(not os.environ.get('SQUALL_ISACOMP_DIR'), reason='set SQUALL_ISACOMP_DIR for RTL execution')
def test_seeded_stalls_repeat_and_change_timing(tmp_path):
    compile_ir('define i32 @main(i32 %a) { %r = mul i32 %a, -7\nret i32 %r }', arguments=[6]).write(tmp_path)
    options = {'variant': 'production', 'memory_latency': 5, 'stall_jitter': 19}
    first = run_rtl(tmp_path, stall_seed=17, **options)
    repeat = run_rtl(tmp_path, stall_seed=17, **options)
    other = run_rtl(tmp_path, stall_seed=91, **options)
    fixed = run_rtl(tmp_path, variant='production', memory_latency=5)
    assert first['result'] == repeat['result'] == other['result'] == fixed['result'] == -42
    assert first['wall_cycles'] == repeat['wall_cycles']
    assert first['wall_cycles'] != other['wall_cycles']
    assert first['wall_cycles'] > fixed['wall_cycles']
