from squall_compiler.rtl import execution_order_matches


MANIFEST = {"entry": "main", "functions": {"main": {"entry_word": 100}}, "root_frame_word": 10}
TRACE = [(1, 10, 1, 7), (2, 10, 1, 35), (100, 10, 0, 0), (101, 10, 0, 42), (102, 10, 0, 42)]


def test_initial_argument_reordering_is_allowed():
    actual = [TRACE[1], TRACE[0], *TRACE[2:]]
    assert execution_order_matches(actual, TRACE, MANIFEST)


def test_reordering_after_function_entry_is_rejected():
    actual = [*TRACE[:3], TRACE[4], TRACE[3]]
    assert not execution_order_matches(actual, TRACE, MANIFEST)


def test_missing_launch_argument_is_rejected():
    assert not execution_order_matches(TRACE[1:], TRACE, MANIFEST)


def test_missing_function_entry_is_rejected():
    assert not execution_order_matches(TRACE[:2], TRACE, MANIFEST)
