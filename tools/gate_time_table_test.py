# gate_time_table_test.py -- caps(), table-shape and slow-set membership
# unit cases for the reference timing table module (gate_time_table.py):
# the work factor scales the watchdog cap together with the budget, short
# references stay pinned at the watchdog floor, the MIN_CAP_S budget floor
# never inverts the clamp, the seven-column row shape round-trips, and the
# slow membership follows the baseline-row seconds rule (D-16/D-17). Entry
# point: gate_time_check_test.py (its main() calls run()). Stdlib only;
# run with -I.

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import (ANY_MANIFEST, SLOW_MIN_REF_S, caps, load_table,
                             resolve_scoped_row, row_line)
# slow_names is the schedule axis' one fast/slow judgment site (D-17);
# this module's table-shape tests exercise it as the table's consumer.
from gate_time_schedule import slow_names
from gate_time_testutil import (CAPS_TABLE, check, close, drop_line,
                                write_table)


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


# One entry row, the two manifest-scope rows and the `default` row of one
# e2e config, so every resolution mode of resolve_scoped_row is visible
# in the returned reference.
SCOPED_TABLE = (
    "# stub scoped table\n"
    "e2e-Debug scoped_entry 5.00 1 e2e-serial - entry row stub\n"
    "e2e-Debug manifest.txt 10.00 1 e2e-serial - manifest scope stub\n"
    "e2e-Debug examples_manifest.txt 20.00 1 e2e-serial - examples "
    "scope stub\n"
    "e2e-Debug default 30.00 1 e2e-serial - default row stub\n")


def test_scoped_row_same_name_hits_its_own_manifest_scope():
    # M-3: resolve_scoped_row must price an entry from the manifest that
    # owns it (the e2e runner is the first real consumer and knows the
    # manifest per entry): same-named entries of different manifests hit
    # their own scope row. ANY_MANIFEST (the checker's default, whose
    # junit carries no manifest info) keeps the legacy fixed order; an
    # explicit manifest name consults that one scope then `default`;
    # None (a subset or renamed override manifest outside the scopes)
    # skips scope rows entirely.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, SCOPED_TABLE)
        rows = load_table(path)
        probe = "unlisted_probe"
        for manifest in ("manifest.txt", "examples_manifest.txt",
                         ANY_MANIFEST, None):
            row = resolve_scoped_row(rows, "e2e-Debug", "scoped_entry",
                                     manifest)
            check(row.ref_s == 5.00,
                  "scoped: the entry row must win for manifest=%r, got "
                  "ref_s=%r" % (manifest, row.ref_s))
        got = {}
        for manifest in ("manifest.txt", "examples_manifest.txt",
                         ANY_MANIFEST, None):
            got[manifest] = resolve_scoped_row(rows, "e2e-Debug", probe,
                                               manifest).ref_s
        check(got["manifest.txt"] == 10.00
              and got["examples_manifest.txt"] == 20.00,
              "scoped: each manifest name must hit its own scope row, got "
              "%r" % (got,))
        check(got[ANY_MANIFEST] == 10.00,
              "scoped: the ANY_MANIFEST default must keep the legacy "
              "nearest-scope order, got %r" % (got[ANY_MANIFEST],))
        check(got[None] == 30.00,
              "scoped: manifest=None must skip scope rows and take "
              "`default`, got %r" % (got[None],))
        # A named manifest with no scope row of its own still lands on
        # `default` (it must not fall through to the other scope rows).
        rows = load_table(write_table(tmp, drop_line(SCOPED_TABLE,
                                                     "e2e-Debug manifest.")))
        row = resolve_scoped_row(rows, "e2e-Debug", probe, "manifest.txt")
        check(row.ref_s == 30.00,
              "scoped: a manifest without its own scope row must fall to "
              "`default`, not to the other scopes, got ref_s=%r"
              % row.ref_s)


def _plain_row(config, entry, ref_s, form="ctest-junit-serial"):
    # Sentinel-less stub row for the membership cases.
    return "%s %s %.2f 1 %s - stub\n" % (config, entry, ref_s, form)


def test_slow_names_derives_membership_from_the_baseline_row():
    # D-16/D-17: the one machine-checkable predicate -- an entry name is
    # slow exactly when its baseline row's reference seconds exceed
    # SLOW_MIN_REF_S (strictly). The baseline row is the ctest-Debug row
    # when present, else the ctest-IDE row (an entry registered only in
    # the IDE tree), else the e2e-Debug row; ASan and Release rows never
    # classify, the coarse-scope rows (`default`, manifest scopes) never
    # classify, and membership is one global list shared by every config
    # rather than a per-config reading.
    check(SLOW_MIN_REF_S == 60,
          "D-16: the fast/slow threshold must be exactly 60 s, got %r"
          % SLOW_MIN_REF_S)
    slow = SLOW_MIN_REF_S + 1.0
    fast = SLOW_MIN_REF_S - 1.0
    gate = float(SLOW_MIN_REF_S)
    table = (
        _plain_row("ctest-Debug", "slow_edge", slow)
        + _plain_row("ctest-Debug", "fast_edge", fast)
        + _plain_row("ctest-Debug", "gate_exact", gate)
        + _plain_row("ctest-IDE", "ide_only_slow", slow)
        + _plain_row("ctest-Debug", "debug_row_wins", fast)
        + _plain_row("ctest-IDE", "debug_row_wins", slow + 30.0)
        + _plain_row("ctest-ASan", "asan_only_heavy", 800.0)
        + _plain_row("ctest-Release", "release_only_heavy", 500.0)
        + _plain_row("e2e-ASan", "e2e_asan_only_heavy", 491.0)
        + _plain_row("e2e-Debug", "e2e_baseline_slow", slow, "e2e-serial")
        + _plain_row("ctest-Debug", "default", 100.0)
        + _plain_row("e2e-Debug", "manifest.txt", 100.0, "e2e-serial"))
    with tempfile.TemporaryDirectory() as tmp:
        members = set(slow_names(load_table(write_table(tmp, table))))
    expected = {"slow_edge", "ide_only_slow", "e2e_baseline_slow"}
    check(members == expected,
          "slow_names: membership must come from the baseline row's "
          "seconds (strictly above the threshold), expected %s, got %s"
          % (sorted(expected), sorted(members)))


def test_load_table_names_the_deleted_category_column():
    # D-17 dropped the category column; a stale eight-column row must not
    # reparse silently (the category token would land in `form`, which no
    # consumer reads), so the row's measurement form is validated against
    # the form vocabulary and the error names the deleted column. A row
    # too short to carry the form errors on the column count instead.
    with tempfile.TemporaryDirectory() as tmp:
        stale = ("ctest-Debug stale_shape 10.00 1 常规 ctest-junit-serial "
                 "- stub\n")
        try:
            load_table(write_table(tmp, stale, name="stale.txt"))
            check(False, "load_table: a stale category-column row must "
                         "raise")
        except ValueError as exc:
            check("category" in str(exc),
                  "load_table: the stale-row error must name the deleted "
                  "category column, got %r" % str(exc))
        short = "ctest-Debug short_row 10.00\n"
        try:
            load_table(write_table(tmp, short, name="short.txt"))
            check(False, "load_table: a row without a form column must "
                         "raise")
        except ValueError as exc:
            check("column" in str(exc),
                  "load_table: the too-short-row error must name the "
                  "columns, got %r" % str(exc))


def test_row_line_prints_the_seven_columns():
    # D-17: a table row carries config, entry, ref_s, ref_work, form,
    # sentinel, reason -- seven columns with the three human columns last,
    # and the sentinel placeholder rule (a reason never rides without a
    # sentinel token in front of it) survives the column removal.
    got = row_line("ctest-Debug", "priced_entry", 12.5, 1,
                   "ctest-junit-serial", "yes", "measured 2026-11-21")
    check(got == "ctest-Debug priced_entry 12.50 1 ctest-junit-serial yes "
                 "measured 2026-11-21",
          "row_line: the seven-column row mismatched, got %r" % got)
    got = row_line("ctest-Debug", "bare_entry", 3.0, 2,
                   "ctest-junit-serial", "", "")
    check(got == "ctest-Debug bare_entry 3.00 2 ctest-junit-serial",
          "row_line: no sentinel and no reason must not trail tokens, "
          "got %r" % got)
    got = row_line("e2e-Debug", "reasoned", 40.0, 1, "e2e-serial", "",
                   "kept for the schedule")
    check(got == "e2e-Debug reasoned 40.00 1 e2e-serial - kept for the "
                 "schedule",
          "row_line: a reason must ride behind a sentinel placeholder, "
          "got %r" % got)


def run():
    test_caps_work_factor_two_doubles_cap_and_budget()
    test_caps_floor_entry_watchdog_pinned_at_120()
    test_caps_clamp_lower_bound_does_not_invert()
    test_scoped_row_same_name_hits_its_own_manifest_scope()
    test_slow_names_derives_membership_from_the_baseline_row()
    test_load_table_names_the_deleted_category_column()
    test_row_line_prints_the_seven_columns()
