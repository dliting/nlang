# gate_time_measure_test.py -- measurement parsing and recording cases
# for the measure module (gate_time_measure.py): the guard-six member
# that --record keeps the four human columns, the max-merge discipline
# on both measurement sources, the run-status-only filter that keeps
# failed/skipped rows out of the reference, and the junit <system-out>
# report channel the D-8 self-report cross-check reads. Entry point:
# gate_time_check_test.py (its main() calls run()). Stdlib only; run
# with -I.

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_measure import (apply_record, junit_reports, parse_junit_rows,
                               parse_measure_rows)
from gate_time_table import load_table
from gate_time_testutil import check, close, write_table


JUNIT_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="ctest">
  <testsuite name=" suite" tests="2">
    <testcase classname="E:/cases/nlang/dev/tests" name="soak_entry"
              status="run" time="250.5"/>
    <testcase name="fresh_missing_entry" status="run" time="12.0"/>
  </testsuite>
</testsuites>
"""


def test_record_preserves_human_columns():
    # Guard six member: a re-record refreshes ref_s and form only; the
    # category, ref_work, sentinel and reason columns must survive
    # verbatim, and a ctest config (no default row) gains rows for
    # measured entries it had none for.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, (
            "# stub recording table\n"
            "ctest-Debug soak_entry 0.00 100 慢功能 ctest-junit-serial - "
            "soak keep reason\n"
            "ctest-Debug stale_row 7.00 1 常规 ctest-junit-serial - "
            "untouched row\n"))
        apply_record(path, "ctest-Debug", parse_junit_rows(JUNIT_XML))
        rows = {(r.config, r.entry): r for r in load_table(path)}
        soak = rows[("ctest-Debug", "soak_entry")]
        check(close(soak.ref_s, 250.5),
              "record: ref_s must take the measured value, got %r"
              % soak.ref_s)
        check(soak.ref_work == 100 and soak.category == "慢功能",
              "record: category/ref_work must survive, got %r" % (soak,))
        check(soak.sentinel == "-" and soak.reason == "soak keep reason",
              "record: sentinel/reason must survive, got %r" % (soak,))
        fresh = rows.get(("ctest-Debug", "fresh_missing_entry"))
        check(fresh is not None and close(fresh.ref_s, 12.0)
              and fresh.category == "常规" and fresh.ref_work == 1,
              "record: a ctest config must gain a row for a new entry, "
              "got %r" % (fresh,))
        stale = rows[("ctest-Debug", "stale_row")]
        check(close(stale.ref_s, 7.0), "record: unmeasured rows must stay")
        text = path.read_text(encoding="utf-8")
        check(text.startswith("# stub recording table"),
              "record: the header comment must survive")


def test_record_measure_group_max_and_default_skip():
    # The runner can emit two MEASURE rows for one entry (pre-existing
    # double-verdict path): merge per name by max, never sum, and a lone
    # row is not an error. An e2e config with a default row covers new
    # names without adding rows.
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, (
            "# stub e2e table\n"
            "e2e-Debug default 10.00 1 常规 e2e-serial - stub default\n"
            "e2e-Debug soak_e2e 5.00 1 慢功能 e2e-serial yes e2e exception "
            "reason\n"))
        meas = parse_measure_rows(
            "PASS soak_e2e (exit=0)\n"
            "TIME soak_e2e 240.00\n"
            "MEASURE soak_e2e 250.00 form=e2e-serial\n"
            "MEASURE soak_e2e 240.00 form=e2e-serial\n"
            "MEASURE soak_e2e 260.00 form=e2e-serial\n"
            "MEASURE brand_new_name 9.00 form=e2e-serial\n"
            "MEASURE lone_entry 3.00 form=e2e-serial\n")
        check("soak_e2e" in meas and close(meas["soak_e2e"][0], 260.0),
              "record: duplicate MEASURE rows must merge by max, got %r"
              % meas)
        apply_record(path, "e2e-Debug", meas)
        by_name = {r.entry: r for r in load_table(path)}
        soak = by_name["soak_e2e"]
        check(close(soak.ref_s, 260.0) and soak.ref_work == 1
              and soak.category == "慢功能" and soak.sentinel == "yes"
              and soak.reason == "e2e exception reason",
              "record: refreshed row must keep the human columns, got %r"
              % (soak,))
        check("brand_new_name" not in by_name
              and "lone_entry" not in by_name,
              "record: a config with a default row must not grow rows for "
              "already-covered names, got %r" % sorted(by_name))


def test_record_junit_group_max_never_sum():
    # The R-4 max-merge discipline pinned on the junit side (the MEASURE
    # side has its own case): same-name run rows merge keeping the
    # maximum reading, never the sum, and a lone row is not an error.
    meas = parse_junit_rows(
        '<testsuites><testsuite name="s" tests="3">'
        '<testcase name="dup_entry" status="run" time="30.0"/>'
        '<testcase name="dup_entry" status="run" time="20.0"/>'
        '<testcase name="lone_entry" status="run" time="5.0"/>'
        '</testsuite></testsuites>')
    check("dup_entry" in meas and close(meas["dup_entry"][0], 30.0),
          "record: duplicate junit run rows must merge by max, never sum, "
          "got %r" % (meas,))
    check("lone_entry" in meas and close(meas["lone_entry"][0], 5.0),
          "record: a lone junit run row must stand on its own, got %r"
          % (meas,))


def test_record_junit_keeps_only_run_status_rows():
    # ref_s means "seconds for one completed reference workload" (R-7),
    # and a real ctest junit (verified against CMake 3.25.2) marks Passed
    # with status="run", Failed and Timeout alike with status="fail" (the
    # timeout time is the then-active cap, not a workload), and Skipped
    # with status="notrun" whose time is only the skip-decision cost. A
    # row without the status attribute is refused too: no reading beats
    # a wrong reading.
    meas = parse_junit_rows(
        '<testsuites><testsuite name="s" tests="6">'
        '<testcase name="mixed_entry" status="run" time="40.0"/>'
        '<testcase name="mixed_entry" status="fail" time="120.0"/>'
        '<testcase name="mixed_entry" status="notrun" time="0.02"/>'
        '<testcase name="timeout_shaped" status="fail" time="120.0"/>'
        '<testcase name="skipped_only" status="notrun" time="0.02"/>'
        '<testcase name="statusless_entry" time="40.0"/>'
        '</testsuite></testsuites>')
    check("mixed_entry" in meas and close(meas["mixed_entry"][0], 40.0),
          "record: only the run row may feed an entry that also has "
          "fail/notrun rows, got %r" % (meas,))
    check("timeout_shaped" not in meas,
          "record: a timeout-shaped fail row (cap-sized time) must not "
          "become a reference, got %r" % (meas,))
    check("skipped_only" not in meas,
          "record: a notrun skip row must not become a reference, got %r"
          % (meas,))
    check("statusless_entry" not in meas,
          "record: a testcase without the status attribute must be "
          "refused, got %r" % (meas,))


def test_reports_channel_reads_system_out_per_entry():
    # The D-8 self-report channel pinned on its reader: junit_reports
    # yields {name: text} for entries whose <system-out> holds text (a
    # passing entry's captured stdout on CMake 3.25.2; a failing entry
    # carries <failure> and <system-out> side by side, which is why the
    # checker reads reports from the same document the timings come from
    # -- the inline BUDGET-EXCEEDED dedup needs exactly that shape),
    # omits silent and empty-report entries so the checker's "absent =
    # did not self-report" reading is exact, and joins multiple chunks
    # with newlines.
    reports = junit_reports(
        '<testsuites><testsuite name="s" tests="5">'
        '<testcase name="soak_entry" status="run" time="250.5">'
        '<system-out>start\nrounds=100\n</system-out></testcase>'
        '<testcase name="silent_entry" status="run" time="2.0"/>'
        '<testcase name="empty_report" status="run" time="3.0">'
        '<system-out></system-out></testcase>'
        '<testcase name="over_budget_entry" status="fail" time="12.0">'
        '<failure message="subprocess killed"/>'
        '<system-out>  BUDGET-EXCEEDED budget=200 actual=240\n'
        '</system-out></testcase>'
        '<testcase name="split_entry" status="run" time="4.0">'
        '<system-out>rounds=</system-out><system-out>200\n</system-out>'
        '</testcase>'
        '</testsuite></testsuites>')
    check(reports.get("soak_entry") == "start\nrounds=100\n",
          "reports: a passing entry's system-out must arrive verbatim, "
          "got %r" % (reports,))
    check("silent_entry" not in reports and "empty_report" not in reports,
          "reports: entries without report text must be absent, got %r"
          % (reports,))
    check("over_budget_entry" in reports
          and "BUDGET-EXCEEDED" in reports["over_budget_entry"],
          "reports: a failing entry's report must survive its <failure> "
          "sibling, got %r" % (reports,))
    check(reports.get("split_entry") == "rounds=\n200\n",
          "reports: multiple chunks must join with newlines, got %r"
          % (reports,))


def run():
    test_record_preserves_human_columns()
    test_record_measure_group_max_and_default_skip()
    test_record_junit_group_max_never_sum()
    test_record_junit_keeps_only_run_status_rows()
    test_reports_channel_reads_system_out_per_entry()
