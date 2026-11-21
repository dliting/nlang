# gate_time_table_test.py -- caps() unit cases for the reference timing
# table module (gate_time_table.py): the work factor scales the watchdog
# cap together with the budget, short references stay pinned at the
# watchdog floor, and the MIN_CAP_S budget floor never inverts the
# clamp. Entry point: gate_time_check_test.py (its main() calls run()).
# Stdlib only; run with -I.

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import caps
from gate_time_testutil import CAPS_TABLE, check, close, write_table


def test_caps_work_factor_two_doubles_cap_and_budget():
    # D-1: the work factor must scale the watchdog cap together with the
    # budget, or factor 2 would let the watchdog kill legitimate long runs.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, CAPS_TABLE)
        b1, w1 = caps("ctest-Debug", "caps_over_floor", table_path=path)
        b2, w2 = caps("ctest-Debug", "caps_over_floor", work_factor=2.0,
                      table_path=path)
        check(close(b1, 200.0) and close(w1, 1000.0),
              "caps: factor 1 must give (200, 1000), got %r" % ((b1, w1),))
        check(close(b2, 400.0) and close(w2, 2000.0),
              "caps: factor 2 must double both, got %r" % ((b2, w2),))


def test_caps_floor_entry_watchdog_pinned_at_120():
    # MIN_WATCHDOG_S floors the cap of short references (a naked product
    # would give 5*10=50); at ref < 12 s even factor 2 stays pinned at the
    # floor -- the guard-five probe needs refs above 12 s to see movement.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, CAPS_TABLE)
        for factor in (1.0, 2.0):
            b, w = caps("ctest-Debug", "caps_small_entry",
                        work_factor=factor, table_path=path)
            check(close(w, 120.0),
                  "caps: a sub-12 s reference must stay floored at 120 "
                  "at factor %r, got %r" % (factor, w))
            check(close(b, 60.0),
                  "caps: its budget must stay floored at MIN_CAP_S at "
                  "factor %r, got %r" % (factor, b))


def test_caps_clamp_lower_bound_does_not_invert():
    # The MIN_CAP_S budget floor must never lift the budget above the
    # (floored) watchdog cap: MIN_WATCHDOG_S = 2 * MIN_CAP_S keeps the
    # clamp well-formed for a one-second reference at the band edge.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, CAPS_TABLE)
        b, w = caps("ctest-Debug", "caps_tiny_entry", table_path=path)
        check(close(b, 60.0) and close(w, 120.0) and b < w,
              "caps: the 1 s entry must clamp to (60, 120) with budget "
              "below cap, got %r" % ((b, w),))
        b, w = caps("ctest-Debug", "caps_tiny_entry",
                    scalar=4.0, table_path=path)
        check(close(b, 60.0) and close(w, 120.0) and b < w,
              "caps: band-edge scalar must not invert the clamp, got %r"
              % ((b, w),))
        b, w = caps("ctest-Debug", "caps_tiny_entry", work_factor=0.5,
                    table_path=path)
        check(close(b, 60.0) and close(w, 120.0),
              "caps: the work ratio floors at 1 (factor 0.5 behaves like "
              "1), got %r" % ((b, w),))
        b, w = caps("ctest-Debug", "caps_over_floor", scalar=2.0,
                    table_path=path)
        check(close(b, 400.0) and close(w, 1000.0),
              "caps: the machine scalar scales the budget only, the "
              "watchdog stays scalar-free, got %r" % ((b, w),))


def run():
    test_caps_work_factor_two_doubles_cap_and_budget()
    test_caps_floor_entry_watchdog_pinned_at_120()
    test_caps_clamp_lower_bound_does_not_invert()
