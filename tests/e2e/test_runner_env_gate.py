#Unit tests for the e2e runner's environment gating (env_gates.txt):
#parse, the pure skip decision, and the probe dispatch. Real
#subprocesses stay out — probes are injected (the runner's probe
#functions touch ping/getaddrinfo, which are machine-state, not test
#assertions).
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_e2e_tests as runner


def test_parse_gates_maps_names_to_keys(tmp_path):
    gates_file = tmp_path / "env_gates.txt"
    gates_file.write_text(
        "# comment line\n"
        "stdlib_net_ping_loopback icmp\n"
        "stdlib_net_ping_error_text dns\n",
        encoding="utf-8")
    gates = runner.parse_env_gates(str(gates_file))
    assert gates == {
        "stdlib_net_ping_loopback": {"icmp"},
        "stdlib_net_ping_error_text": {"dns"},
    }


def test_parse_gates_missing_file_is_empty_map(tmp_path):
    assert runner.parse_env_gates(str(tmp_path / "absent.txt")) == {}


def test_parse_gates_unknown_capability_fails_loud(tmp_path):
    gates_file = tmp_path / "env_gates.txt"
    gates_file.write_text("some_entry wifi\n", encoding="utf-8")
    with pytest.raises(ValueError, match="wifi"):
        runner.parse_env_gates(str(gates_file))


def test_parse_gates_keyless_row_fails_loud(tmp_path):
    gates_file = tmp_path / "env_gates.txt"
    gates_file.write_text("some_entry\n", encoding="utf-8")
    with pytest.raises(ValueError, match="some_entry"):
        runner.parse_env_gates(str(gates_file))


def test_skip_decision_gated_and_missing():
    gates = {"entry_a": {"icmp"}}
    env_state = {"icmp": False}
    assert runner.env_skip_reason("entry_a", gates, env_state) == \
        "env icmp unavailable"


def test_skip_decision_gated_and_present():
    gates = {"entry_a": {"icmp"}}
    env_state = {"icmp": True}
    assert runner.env_skip_reason("entry_a", gates, env_state) is None


def test_skip_decision_ungated_entry_runs_everywhere():
    assert runner.env_skip_reason("entry_b", {}, {"icmp": False}) is None


def test_skip_decision_multi_key_reports_only_missing():
    gates = {"entry_c": {"dns", "icmp"}}
    env_state = {"dns": True, "icmp": False}
    assert runner.env_skip_reason("entry_c", gates, env_state) == \
        "env icmp unavailable"


def test_probe_env_dispatches_once_per_key():
    calls = []

    def fake_icmp():
        calls.append("icmp")
        return False

    def fake_dns():
        calls.append("dns")
        return True

    original = dict(runner.ENV_PROBES)
    runner.ENV_PROBES = {"icmp": fake_icmp, "dns": fake_dns}
    try:
        state = runner.probe_env({"icmp", "dns"})
    finally:
        runner.ENV_PROBES = original
    assert state == {"icmp": False, "dns": True}
    assert sorted(calls) == ["dns", "icmp"]
