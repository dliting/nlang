# gate_time_timeouts_test.py -- guard-five cases for the registered-cap
# axis module (gate_time_timeouts.py): registered TIMEOUT properties
# against the table-derived watchdog caps, driven through the
# check-timeouts CLI against a fake tree directory and a saved ctest
# --show-only document (fresh and stale dispositions, the multi-key
# max rule, the floored entry) plus the readers pinned at function
# level. Entry point: gate_time_check_test.py (its main() calls run()).
# Stdlib only; run with -I.

import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_table import load_table
from gate_time_testutil import check, write_table
from gate_time_timeouts import (check_timeouts, expected_watchdogs,
                                read_timeouts, threshold_inputs)


def _show_only_doc(entries):
    """A saved ctest --show-only=json-v1 document for the guard-five CLI
    cases: (name, timeout-or-None) pairs, None meaning the entry carries
    no TIMEOUT property at all."""
    tests = []
    for name, timeout in entries:
        props = ([{"name": "TIMEOUT", "value": str(timeout)}]
                 if timeout is not None else [])
        tests.append({"name": name, "properties": props})
    return json.dumps({"tests": tests})


def _timeouts_tree(tmp, configs="ctest-Debug", work_factor=1.0):
    """A configured-tree directory for the guard-five CLI: the generated
    thresholds header (its own header says which config keys and work
    factor the caps were derived from -- the checker replays those rather
    than guessing from the shell's environment) plus the
    CTestTestfile.cmake stamp the freshness predicate reads. Writing the
    stamp after the table reads fresh (same-tick timestamps count as
    fresh); the stale direction is pinned explicitly per case with an
    os.utime rewind, because this platform's file-mtime tick (~1 ms)
    makes write-order alone a racy witness."""
    tree = Path(tmp) / "tree"
    (tree / "tests").mkdir(parents=True, exist_ok=True)
    (tree / "gate_time_thresholds.cmake").write_text(
        "# stub thresholds\n# configs: %s\n# work_factor: %r\n"
        % (configs, work_factor), encoding="utf-8")
    (tree / "CTestTestfile.cmake").write_text("# stamp\n", encoding="utf-8")
    return tree


def _run_check_timeouts(tmp, table, tree, doc):
    saved = Path(tmp) / "show_only.json"
    saved.write_text(doc, encoding="utf-8")
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = run_cli(["check-timeouts", "--tree", str(tree),
                      "--show-only", str(saved), "--table", str(table)])
    return rc, buf.getvalue()


def test_timeouts_fresh_caps_match_including_the_floored_entry():
    # Guard five green half: every registered TIMEOUT equals the
    # table-derived watchdog cap. zeta pins the floored shape -- ref 5.00
    # caps at MIN_WATCHDOG_S (120.0), where the bare ref x 10 product
    # (50.0) would misjudge the correct registration red, exactly the
    # misjudge D-1's full formula exists to prevent. gamma carries no
    # property and has no row: absence on both sides is agreement.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub guard-five table (gate_time_check selftest)\n"
            "ctest-Debug alpha_probe_tests 100.00 1 常规 ctest-junit-"
            "serial - stub\n"
            "ctest-Debug zeta_probe_tests 5.00 1 常规 ctest-junit-serial"
            " - stub\n"))
        doc = _show_only_doc([("alpha_probe_tests", 1000.0),
                              ("zeta_probe_tests", 120.0),
                              ("gamma_probe_tests", None)])
        rc, out = _run_check_timeouts(tmp, table, _timeouts_tree(tmp), doc)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "timeouts: caps derived from the table must pass, got rc=%r "
              "out=%r" % (rc, out))


def test_timeouts_multi_key_tree_wants_the_largest_cap():
    # A multi-config tree registers ONE TIMEOUT per entry that has to
    # hold for every config key the tree was configured with, so the
    # expected value is the largest over the keys -- the same max rule
    # emit_cmake applied when it wrote the caps file. Debug prices 1000
    # and Release 600: the registration passes at 1000, and 600 must go
    # red naming both numbers -- a checker reading one column only would
    # green-light the wrong cap here.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub guard-five table (gate_time_check selftest)\n"
            "ctest-Debug dual_key_entry 100.00 1 常规 ctest-junit-serial"
            " - stub\n"
            "ctest-Release dual_key_entry 60.00 1 性能 ctest-junit-serial"
            " - stub\n"))
        tree = _timeouts_tree(tmp, configs="ctest-Debug ctest-Release")
        rc, out = _run_check_timeouts(
            tmp, table, tree, _show_only_doc([("dual_key_entry", 1000.0)]))
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "timeouts: the largest cap over the key set must pass, got "
              "rc=%r out=%r" % (rc, out))
        rc, out = _run_check_timeouts(
            tmp, table, tree, _show_only_doc([("dual_key_entry", 600.0)]))
        check(rc == 1 and "TIMEOUT dual_key_entry registered=600.0 "
              "want=1000.0" in out,
              "timeouts: a single-key cap registered on a multi-key tree "
              "must red with both numbers, got rc=%r out=%r" % (rc, out))


def test_timeouts_stale_configure_recommends_one_reconfigure():
    # The disposition split's stale half: caps older than the current
    # table are the previous configure's, and the right next step is one
    # re-run configure (only CTestTestfile.cmake regenerates, nothing
    # recompiles) -- not a per-entry hand-edit list the human chokes on.
    # Rewinding the stamp's mtime behind the table's is the staleness
    # witness _tree_is_fresh reads (write order alone would race the
    # ~1 ms mtime tick).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub guard-five table (gate_time_check selftest)\n"
            "ctest-Debug alpha_probe_tests 100.00 1 常规 ctest-junit-"
            "serial - stub\n"))
        tree = _timeouts_tree(tmp)
        past = table.stat().st_mtime - 1000.0
        os.utime(tree / "CTestTestfile.cmake", (past, past))
        rc, out = _run_check_timeouts(
            tmp, table, tree, _show_only_doc([("alpha_probe_tests", 900.0)]))
        check(rc == 1 and "GATE gate_time_check: FAIL (1 findings)" in out,
              "timeouts: a stale tree folds every mismatch into one "
              "finding, got rc=%r out=%r" % (rc, out))
        check("re-run configure" in out and "CTestTestfile.cmake" in out,
              "timeouts: the stale wording must name the remedy and what "
              "regenerates, got out=%r" % out)
        check("TIMEOUT alpha_probe_tests registered=" not in out,
              "timeouts: the stale path must not edit-walk the entries, "
              "got out=%r" % out)


def test_timeouts_fresh_mismatch_names_each_entry():
    # The split's fresh half: with the configure current, a mismatch can
    # only be a hand-written TIMEOUT property that outlived the table cap
    # (or was set beside one), so each entry is named with both numbers,
    # and an entry carrying a property with no row at all reads
    # registered=120.0 want=none -- the property delete is the whole fix.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub guard-five table (gate_time_check selftest)\n"
            "ctest-Debug alpha_probe_tests 100.00 1 常规 ctest-junit-"
            "serial - stub\n"))
        doc = _show_only_doc([("alpha_probe_tests", 900.0),
                              ("extra_capped_entry", 120.0)])
        rc, out = _run_check_timeouts(tmp, table, _timeouts_tree(tmp), doc)
        check(rc == 1 and "GATE gate_time_check: FAIL (2 findings)" in out,
              "timeouts: a fresh tree names every mismatch separately, got "
              "rc=%r out=%r" % (rc, out))
        check("TIMEOUT alpha_probe_tests registered=900.0 want=1000.0"
              in out and "TIMEOUT extra_capped_entry registered=120.0 "
                         "want=none" in out,
              "timeouts: both findings must carry their numbers, got "
              "out=%r" % out)


def test_timeouts_readers_and_the_site_hook():
    # The guard-five readers pinned at the function level: the thresholds
    # header refuses a file that did not come from the cap emitter naming
    # the header it lacks, the show-only extraction maps a missing
    # property to None, expected_watchdogs maxes over the keys and
    # follows the work factor (default rows price scopes, not entries,
    # and stay out), and check_timeouts' site hook appends the line to
    # delete on a fresh-configure finding.
    configs, factor = threshold_inputs(
        "# stub\n# configs: ctest-Debug ctest-Release\n"
        "# work_factor: 2.0\n")
    check(configs == ["ctest-Debug", "ctest-Release"] and factor == 2.0,
          "timeouts: the header must round-trip, got %r"
          % ((configs, factor),))
    for label, want in (("# configs: ctest-Debug\n", "work_factor"),
                        ("# work_factor: 1.0\n", "configs")):
        try:
            threshold_inputs(label)
            check(False, "timeouts: a header missing %s must raise" % want)
        except ValueError as exc:
            check(want in str(exc),
                  "timeouts: the refusal must name the missing header %s, "
                  "got %r" % (want, exc))
    registered = read_timeouts(_show_only_doc([("alpha_probe_tests", 1000.0),
                                               ("plain_entry", None)]))
    check(registered == {"alpha_probe_tests": 1000.0, "plain_entry": None},
          "timeouts: property values and their absence must read as "
          "numbers and None, got %r" % (registered,))
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub guard-five table (gate_time_check selftest)\n"
            "ctest-Debug alpha_probe_tests 100.00 1 常规 ctest-junit-"
            "serial - stub\n"
            "ctest-Debug default 500.00 1 常规 ctest-junit-serial - stub\n"))
        rows = load_table(table)
        check(expected_watchdogs(rows, ["ctest-Debug"], 1.0)
              == {"alpha_probe_tests": 1000.0},
              "timeouts: the expected set is per-entry caps with default "
              "rows skipped, got %r" % (expected_watchdogs(
                  rows, ["ctest-Debug"], 1.0),))
        check(expected_watchdogs(rows, ["ctest-Debug"], 2.0)
              == {"alpha_probe_tests": 2000.0},
              "timeouts: a doubled work factor doubles the cap the tree "
              "was configured with")
    ok, findings = check_timeouts({"alpha_probe_tests": 900.0},
                                  {"alpha_probe_tests": 1000.0}, True,
                                  site_of=lambda name:
                                  "tests/CMakeLists.txt:99")
    check(not ok and len(findings) == 1 and "want=1000.0" in findings[0]
          and "; site tests/CMakeLists.txt:99" in findings[0],
          "timeouts: the site hook must append the line to delete, got "
          "%r" % (findings,))


def run():
    test_timeouts_fresh_caps_match_including_the_floored_entry()
    test_timeouts_multi_key_tree_wants_the_largest_cap()
    test_timeouts_stale_configure_recommends_one_reconfigure()
    test_timeouts_fresh_mismatch_names_each_entry()
    test_timeouts_readers_and_the_site_hook()
