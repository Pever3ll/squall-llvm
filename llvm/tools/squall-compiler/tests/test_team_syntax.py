import pytest
from squall_compiler.assembler import AssemblyError, Program, assemble


@pytest.mark.parametrize('team,legacy', [
    ('add.r (0x58).l.r [0x4]\nb: .word 0', 'dyadic op=Add match=Read dest=(0x58).L second=b.R addr=frame:4\nb: .word value=0'),
    ('mul.c end.r (data)', 'dyadic op=Mul match=Const dest=end.R addr=code:data'),
    ('sub (0).l (-2)', 'dyadic op=Sub match=Once dest=(0).L addr=code:(-2)'),
    ('dupr.r halt.l 17', 'dyadic op=Dupr match=Read dest=halt.L addr=abs:17'),
    ('add end.l #7', 'monadic op=Add dest=end.L imm=7'),
    ('send.r halt.l [0]', 'send dest=halt.L addr=frame:0 match=Read'),
    ('extract.r end.l a.r [0]', 'extract dest=end.L target=a.R addr=frame:0 match=Read'),
    ('write 0x1', 'write addr=1'),
    ('.slot -7', '.slot value=-7'),
])
def test_team_spelling_preserves_encoding(team, legacy):
    suffix = '\nend: nop halt.l #0\ndata: .slot 42'
    expected = assemble('a: ' + legacy + suffix)
    parsed = Program.parse('a: ' + team + suffix)
    assert assemble(parsed) == expected
    assert assemble(parsed.text()) == expected


def test_separate_secondary_port_alias_and_comments():
    a = 'entry: add.r halt.l.r [4] ; second executes the following record\nnext: write 1'
    b = 'entry: add.r halt.l r [4]\nnext: write 1'
    assert assemble(a) == assemble(b)
    assert '#7' in Program.parse('a: add halt.l #7 ; immediate').text()


@pytest.mark.parametrize('body,error', [
    ('add.x halt.l [0]', 'matching suffix'),
    ('add.r halt.l #7', 'immediates'),
    ('send.r halt.l.r [0]', 'secondary'),
    ('extract.r halt.l [0]', 'target tag'),
    ('add halt.l.r #0', 'next word'),
    ('add.r halt.l [2048]', 'unsigned 11'),
    ('add.r halt.l (1024)', 'signed 11'),
    ('add halt.l #2147483648', 'signed 32'),
    ('add halt.l.l r [0]', 'duplicate secondary'),
    ('add halt.l [0] unexpected', 'unexpected operands'),
])
def test_malformed_team_syntax_fails(body, error):
    with pytest.raises(AssemblyError, match=error):
        assemble('entry: ' + body)


def test_unlabelled_records_receive_collision_free_labels():
    source = '_line2: dupr output.l #7\nadd output.l #35\noutput: write 1'
    program = Program.parse(source)
    assert program.records[1].label != '_line2'
    assert assemble(program) == assemble(program.text())


def test_port_suffix_label_is_rejected_as_ambiguous():
    with pytest.raises(AssemblyError, match='port suffix'):
        assemble('target.l: nop halt.l #0')


def test_handwritten_assembly_executes_on_reference(tmp_path):
    from pathlib import Path
    from squall_compiler.assembler import token_words
    from squall_compiler.compiler import Image
    from squall_compiler.reference import run_image
    source = Path(__file__).resolve().parents[1] / 'examples/assembly_add.sqa'
    program = Program.parse(source.read_text())
    words, labels = assemble(program)
    stack_base = 4096 - 16
    low, high = token_words(labels['entry'], 0)
    words.update({stack_base // 8: low, stack_base // 8 + 1: high})
    manifest = {'format': 'squall-v2-image-1', 'memory_bytes': 4096,
                'stack_base': stack_base, 'stack_top': stack_base - 16,
                'result_word': 1, 'status_word': 2, 'stack_token_capacity': 2,
                'max_cycles': 1000}
    Image(program, words, labels, manifest).write(tmp_path)
    result = run_image(tmp_path, trace=True)
    assert result['quiescent'] and result['status'] == 1 and result['result'] == 42


def test_legacy_data_comment_is_preserved():
    assert assemble('a: .slot value=42 # old comment')[0] == assemble('a: .slot 42 ; new comment')[0]
