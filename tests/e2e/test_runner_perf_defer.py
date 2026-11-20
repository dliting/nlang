#Unit tests for the e2e runner's performance defer list
#(NLANG_E2E_EXCLUDE): the parse, the loud failures, and the pure defer
#decision. Same discipline as test_runner_env_gate.py — pure functions,
#no subprocesses.
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_e2e_tests as runner


def test_parse_perf_defer_maps_file_to_names(tmp_path):
    defer_file = tmp_path / "gate_perf_defer.txt"
    defer_file.write_text(
        "# temporary perf trim — remove after the test-time rework\n"
        "stdlib_net_ping_stress\n"
        "concurrent_gate_hammer\n",
        encoding="utf-8")
    names = runner.parse_perf_defer(str(defer_file))
    assert names == {"stdlib_net_ping_stress", "concurrent_gate_hammer"}


def test_parse_perf_defer_missing_file_fails_loud(tmp_path):
    #The env var naming the file is an explicit trim intent; a missing
    #file would silently restore the full suite, so it must fail.
    with pytest.raises(ValueError, match="not found"):
        runner.parse_perf_defer(str(tmp_path / "absent.txt"))


def test_validate_perf_defer_rejects_unknown_names():
    known = {"stdlib_net_ping_stress": 1}
    with pytest.raises(ValueError, match="stale_entry"):
        runner.validate_perf_defer({"stale_entry"}, known)


def test_validate_perf_defer_passes_on_known_names():
    known = {"stdlib_net_ping_stress": 1, "other": 1}
    #A superset of the defer list is the normal case — no raise.
    runner.validate_perf_defer({"stdlib_net_ping_stress"}, known)


def test_defer_decision_listed_and_unlisted():
    assert runner.perf_defer_reason(
        "stdlib_net_ping_stress", {"stdlib_net_ping_stress"}) == "perf defer"
    assert runner.perf_defer_reason("plain_entry", set()) is None
