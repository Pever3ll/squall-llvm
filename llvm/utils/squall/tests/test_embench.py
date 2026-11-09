"""embench.py's verdict: a run passes only if the host build, the
interpreter, and every RTL core that ran agree. A fake Embench tree stands
in for the real one, which is not part of this repository."""
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import BIN, clang, requires_llvm

pytestmark = requires_llvm
SCRIPT = Path(__file__).resolve().parents[1] / "embench.py"
SUPPORT = """#define LOCAL_SCALE_FACTOR 7
static int sum;
void initialise_benchmark(void) { sum = 0; }
void warm_caches(int heat) { (void)heat; }
int benchmark(void) {
  for (int i = 0; i < LOCAL_SCALE_FACTOR * CPU_MHZ; ++i)
    sum += i;
  return sum;
}
int verify_benchmark(int result) { return result == EXPECTED; }
"""


def fake_embench(tmp_path, expected):
    root = tmp_path / "embench"
    (root / "src/tiny").mkdir(parents=True)
    (root / "support").mkdir()
    # One repetition (the scale factor becomes 1) sums only i = 0.
    (root / "src/tiny/tiny.c").write_text('#include "support.h"\n' + SUPPORT.replace("EXPECTED", str(expected)))
    (root / "support/support.h").write_text(
        '#include "boardsupport.h"\nvoid initialise_board(void);\nvoid start_trigger(void);\n'
        'void stop_trigger(void);\nvoid initialise_benchmark(void);\nvoid warm_caches(int);\n'
        'int benchmark(void);\nint verify_benchmark(int);\n')
    (root / "support/beebsc.h").write_text("")
    (root / "support/beebsc.c").write_text("int beebsc_unused;\n")
    return root


def run(tmp_path, root, *options):
    return subprocess.run([sys.executable, str(SCRIPT), "--embench", str(root), "--llvm-bin", str(BIN),
                           "--output", str(tmp_path / "out"), *options], capture_output=True, text=True,
                          cwd=SCRIPT.parent)


@pytest.mark.skipif(not clang(), reason="the built Squall Clang is required")
def test_passing_benchmark_exits_zero(tmp_path):
    result = run(tmp_path, fake_embench(tmp_path, 0))
    assert result.returncode == 0 and "1 of 1 benchmark runs passed" in result.stdout, result.stdout + result.stderr


@pytest.mark.skipif(not clang(), reason="the built Squall Clang is required")
def test_failed_self_check_exits_nonzero(tmp_path):
    result = run(tmp_path, fake_embench(tmp_path, 1))
    assert result.returncode == 1 and "0 of 1 benchmark runs passed" in result.stdout, result.stdout


@pytest.mark.skipif(not clang(), reason="the built Squall Clang is required")
def test_rtl_errors_fail_the_run(tmp_path):
    broken = tmp_path / "not-isacomp"
    broken.mkdir()
    result = run(tmp_path, fake_embench(tmp_path, 0), "--isacomp-dir", str(broken))
    assert result.returncode == 1 and "0 of 1 benchmark runs passed" in result.stdout, result.stdout
    assert '"baseline": "error' in result.stdout
