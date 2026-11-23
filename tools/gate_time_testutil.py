# gate_time_testutil.py -- helpers shared by the gate-timing self-test
# corpus (gate_time_check_test.py and its companion case modules). The
# `failures` list and `check` are the corpus's verdict plumbing: every
# case module appends through check, and the entry's main() prints the
# FAIL lines and the verdict line from this one list. The table writers
# and CAPS_TABLE serve several case modules, so they live here rather
# than in any one of them. Stdlib only; run with -I.

from pathlib import Path

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def close(a, b):
    return abs(a - b) < 1e-6


def write_table(tmp, text, name="reference.txt"):
    path = Path(tmp) / name
    path.write_text(text, encoding="utf-8")
    return path


def drop_line(text, prefix):
    lines = text.splitlines()
    kept = [ln for ln in lines if not ln.startswith(prefix)]
    assert len(kept) < len(lines), "stub line %r not found" % prefix
    return "\n".join(kept) + "\n"


# Stub table for the caps() unit cases and the caps/record CLI cases
# (two consumers: gate_time_table_test.py and the entry corpus). The
# 100.00/5.00/1.00 references exercise the above-floor, watchdog-floored
# and budget-clamped paths of D-1.
CAPS_TABLE = (
    "# stub caps table\n"
    "ctest-Debug caps_over_floor 100.00 1 ctest-junit-serial - stub\n"
    "ctest-Debug caps_small_entry 5.00 1 ctest-junit-serial - stub\n"
    "ctest-Debug caps_tiny_entry 1.00 1 ctest-junit-serial - stub\n")
