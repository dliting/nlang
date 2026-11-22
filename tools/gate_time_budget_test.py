# gate_time_budget_test.py -- post-hoc budget cases for the check-budget
# subcommand (gate_time_guards.check_budget via the CLI): the entry-row
# red half, the TIMEOUT_WATCHDOG wording and its anti-double-verdict
# clause, the coarse-scope advisory grading of D-3/R-17, the D-15
# ratchet corridors, guard ten's form refusal, and the D-8 self-report
# cross-check with its inline BUDGET-EXCEEDED dedup. Entry point:
# gate_time_check_test.py (its main() calls run()). Stdlib only; run
# with -I.

import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_table import SAFETY_MULTIPLIER
from gate_time_testutil import check, write_table

# Stub table for the post-hoc budget checker (guard three and the scope
# grading of R-17/D-15). Entry refs of 100.00 give a budget of 200 and a
# watchdog cap of 1000 at scalar 1; the e2e aggregate rows (10.00) give
# the MIN_CAP_S-floored budget of 60 (D-5: the advisory must quote the
# very number the caps() consumer judges against, floor included). Every
# reading below is hand-picked against those numbers, so the corpus stays
# deterministic (no wall-clock dependency).
BUDGET_TABLE = (
    "# stub budget table (gate_time_check selftest)\n"
    "ctest-Debug budget_over_entry 100.00 1 常规 ctest-junit-serial - over stub\n"
    "ctest-Debug budget_under_entry 100.00 1 常规 ctest-junit-serial - under stub\n"
    "ctest-Debug budget_timeout_entry 100.00 1 常规 ctest-junit-serial - killed stub\n"
    "ctest-Debug budget_failed_entry 100.00 1 常规 ctest-junit-serial - failed stub\n"
    "ctest-Debug budget_form_entry 100.00 1 常规 ctest-junit-serial - form stub\n"
    "ctest-Debug budget_flat_entry 100.00 1 常规 ctest-junit-serial - scalar stub\n"
    "ctest-Debug budget_ratchet_mid_entry 100.00 1 常规 ctest-junit-serial - scalar ratchet stub\n"
    "ctest-Debug budget_ratchet_low_entry 100.00 1 常规 ctest-junit-serial - scalar ratchet stub\n"
    "ctest-Debug budget_ratchet_ratio_entry 100.00 1 常规 ctest-junit-serial - ratio ratchet stub\n"
    "ctest-Debug budget_ratchet_high_entry 100.00 1 常规 ctest-junit-serial - ratio ratchet stub\n"
    "ctest-Debug budget_rounds_ok_entry 100.00 100 常规 ctest-junit-serial - rounds stub\n"
    "ctest-Debug budget_rounds_missing_entry 100.00 100 常规 ctest-junit-serial - rounds stub\n"
    "e2e-Debug default 10.00 1 常规 e2e-serial - coarse scope stub\n"
    "e2e-ASan manifest.txt 10.00 1 常规 e2e-serial - manifest scope stub\n")


def _junit_xml(cases):
    """Build a junit document in the CMake 3.25.2 shape the checker must
    parse (status="run" completed, status="fail" for both Failed and
    Timeout — no attribute tells them apart). A 4th tuple element adds
    that testcase's <system-out> text: the channel the D-8 self-report
    and an inline BUDGET-EXCEEDED verdict travel on."""
    body = ""
    for case in cases:
        name, status, secs = case[:3]
        if len(case) > 3:
            body += ('<testcase name="%s" status="%s" time="%.4f">'
                     '<system-out>%s</system-out></testcase>'
                     % (name, status, secs, case[3]))
        else:
            body += ('<testcase name="%s" status="%s" time="%.4f"/>'
                     % (name, status, secs))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<testsuites><testsuite name="s" tests="%d">%s</testsuite>'
            '</testsuites>' % (len(cases), body))


def _run_check_budget(table_path, config, cases, extra=None):
    with tempfile.TemporaryDirectory() as tmp:
        xml = Path(tmp) / "junit.xml"
        xml.write_text(_junit_xml(cases), encoding="utf-8")
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["check-budget", "--junit", str(xml),
                          "--config", config, "--table", str(table_path)]
                         + list(extra or []))
        return rc, buf.getvalue()


def test_check_budget_over_entry_row_red():
    # Guard three hard half (D-1): a completed run over its entry-row
    # budget reads red with the TIMEOUT_BUDGET cause token. Budget for a
    # 100.00 reference at scalar 1 is 200, so 201 is over.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_over_entry", "run", 201.0)])
        check(rc == 1 and "GATE gate_time_check: FAIL" in out,
              "budget: an over-budget entry row must go red, got rc=%r "
              "out=%r" % (rc, out))
        check("BUDGET-FAIL budget_over_entry budget=200.00 actual=201.00 "
              "(TIMEOUT_BUDGET)" in out,
              "budget: the red line must carry the D-1 numbers and the "
              "TIMEOUT_BUDGET cause, got out=%r" % out)


def test_check_budget_timeout_watchdog_wording_once():
    # D-2's wording priority and its anti-double-verdict clause: a fail row
    # sitting at the computed watchdog cap (100*10=1000 here, the shape the
    # timeout probe captured on CMake 3.25.2) is a killed entry — it gets
    # the TIMEOUT_WATCHDOG line and nothing else (no budget verdict behind
    # a reading that is only the kill cap).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_timeout_entry", "fail",
                                      1000.0)])
        check(rc == 1 and "BUDGET budget_timeout_entry TIMEOUT_WATCHDOG"
              in out,
              "budget: a cap-sized fail row must read as the watchdog "
              "wording red, got rc=%r out=%r" % (rc, out))
        check("BUDGET-FAIL budget_timeout_entry" not in out,
              "budget: the killed entry must not be judged by budget too, "
              "got out=%r" % out)
        check("advisory: budget_timeout_entry" not in out,
              "budget: the killed entry must not get a ratchet advisory "
              "either, got out=%r" % out)


def test_check_budget_failed_row_informational_only():
    # A failed run that was not killed contributes no workload reading
    # (lit's negative-time rationale, D-14): no budget verdict, and no
    # ratchet advisory — 5.0 is under the 100.0 half-budget line, but a
    # failed early exit is not evidence to lower the reference.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_failed_entry", "fail", 5.0)])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "budget: a plain failure must not turn the checker red (the "
              "leg's own verdict owns it), got rc=%r out=%r" % (rc, out))
        check("BUDGET budget_failed_entry FAILED" in out,
              "budget: the failed entry must still be listed for the human "
              "triage, got out=%r" % out)
        check("advisory: budget_failed_entry" not in out,
              "budget: a failed run must not fire the ratchet, got "
              "out=%r" % out)


def test_check_budget_run_and_fail_coexist_watchdog_wins():
    # Task 8's --repeat shape: one entry can complete a pass (run row)
    # and get killed at the cap (fail row) in the same leg. The fail row
    # owns the entry's wording -- judging the run row alone would drop
    # the TIMEOUT_WATCHDOG finding and its triage line (and the passing
    # reading must not add a second verdict to the same entry).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_timeout_entry", "run",
                                      300.0),
                                     ("budget_timeout_entry", "fail",
                                      1000.0)])
        check(rc == 1 and "BUDGET budget_timeout_entry TIMEOUT_WATCHDOG"
              in out,
              "budget: a run row beside a killed fail row must read the "
              "watchdog wording, got rc=%r out=%r" % (rc, out))
        check("BUDGET-FAIL budget_timeout_entry" not in out,
              "budget: the coexisting run row must not also judge the "
              "entry by budget (D-2's no-double-verdict), got out=%r"
              % out)


def test_check_budget_default_row_over_budget_advisory_only():
    # Paired with test_check_budget_over_entry_row_red on purpose (the
    # brief pins the pair): an over-budget reading resolved through the
    # coarse `default` row (65 over the 60.00 floored budget) demotes to
    # advisory (D-3/R-17 scope grading) -- an implementation that demotes
    # the whole table would pass one and fail the other.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "e2e-Debug",
                                    [("any_e2e_entry", "run", 65.0)])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "budget: a default-row scope must not go red, got rc=%r "
              "out=%r" % (rc, out))
        check("advisory: any_e2e_entry 超预算 budget=60.00 actual=65.00"
              in out,
              "budget: the aggregate over-budget advisory must carry the "
              "D-1 numbers, got out=%r" % out)
        check("BUDGET-FAIL" not in out,
              "budget: no entry row may be red here, got out=%r" % out)


def test_check_budget_manifest_row_over_budget_advisory_only():
    # The manifest-scope half of the same grading (R-17): with no default
    # row, resolution falls to the manifest-scope row and the over-budget
    # reading stays advisory.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "e2e-ASan",
                                    [("scope_e2e_entry", "run", 65.0)])
        check(rc == 0
              and "advisory: scope_e2e_entry 超预算 budget=60.00 "
                  "actual=65.00" in out
              and "BUDGET-FAIL" not in out,
              "budget: a manifest-row scope must not go red, got rc=%r "
              "out=%r" % (rc, out))


def test_check_budget_manifest_row_beats_default_row():
    # D-3 pins the scope priority to entry row > manifest row > default
    # row (nearest scope wins). The stub gives one config both a manifest
    # row (ref 40, budget 80) and a default row (ref 10, budget 60); an
    # 85.0 reading is over both, and the advisory must quote the manifest
    # row's number -- an implementation that tries `default` first quotes
    # 60.00 here.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, (
            "# stub scope-priority table (gate_time_check selftest)\n"
            "e2e-Debug default 10.00 1 常规 e2e-serial - coarse default stub\n"
            "e2e-Debug manifest.txt 40.00 1 常规 e2e-serial - manifest"
            " row stub\n"))
        rc, out = _run_check_budget(table, "e2e-Debug",
                                    [("scope_win_entry", "run", 85.0)])
        check(rc == 0 and "advisory: scope_win_entry 超预算 budget=80.00 "
                          "actual=85.00" in out,
              "budget: the manifest row must win over the default row "
              "(D-3 nearest scope), got rc=%r out=%r" % (rc, out))
        check("budget=60.00" not in out,
              "budget: the default row must not be consulted once the "
              "manifest row matches, got out=%r" % out)
        check("MISSING" not in out,
              "budget: a coarse-resolved entry must not report missing, "
              "got out=%r" % out)


def test_check_budget_under_half_entry_row_advisory_only():
    # D-15 lower-only ratchet on an entry row: 50.0 under the
    # reference-based line (100*2/2 = 100) yields the advisory; it never
    # turns red.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_under_entry", "run", 50.0)])
        check(rc == 0 and "advisory: budget_under_entry 建议下调参考" in out
              and "GATE gate_time_check: PASS" in out,
              "budget: below the ratchet line must advisory, not red, got "
              "rc=%r out=%r" % (rc, out))


def test_check_budget_aggregate_ratchet_once_with_max_reading():
    # D-15's aggregate-scope frequency rule: the coarse row would fire per
    # entry every round (grinding the coarse design back to fine), so the
    # aggregate scope prints ONE advisory naming its largest reading.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "e2e-Debug",
                                    [("agg_low_a", "run", 2.0),
                                     ("agg_low_b", "run", 3.0),
                                     ("agg_low_c", "run", 1.0)])
        check(rc == 0, "budget: aggregate ratchet readings must not red "
              "the checker, got rc=%r out=%r" % (rc, out))
        lines = [ln for ln in out.splitlines() if "建议下调参考" in ln]
        check(len(lines) == 1,
              "budget: the aggregate scope prints one advisory, got "
              "%r" % (lines,))
        if len(lines) == 1:
            check("agg_low_b" in lines[0] and "max_actual=3.00" in lines[0],
                  "budget: the single advisory must name the scope's "
                  "largest reading, got %r" % lines[0])


def test_check_budget_form_mismatch_red():
    # Guard ten: the reading's timing form (--parallel 2 here) must match
    # the row's recorded measurement form, or a serial reference is being
    # compared against parallel contention (D-2's last paragraph).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_form_entry", "run", 100.0)],
                                    extra=["--parallel", "2"])
        check(rc == 1 and "FORM ctest-Debug budget_form_entry" in out,
              "budget: a form mismatch must go red, got rc=%r out=%r"
              % (rc, out))
        check("ctest-junit-serial" in out
              and "ctest-junit-parallel-2" in out and "re-record" in out,
              "budget: the form finding must show both forms and the "
              "re-record hint, got out=%r" % out)


def test_check_budget_missing_rows_are_report_lines():
    # A junit entry with no resolvable row cannot be judged at all (and
    # for ctest configs the absence itself is guard-one territory at
    # recording time); the checker reports the set difference and leaves
    # the verdict to the rows it could judge (the table is the single
    # truth — the checker never invents a budget for an unknown entry).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_untracked_run", "run", 5.0),
                                     ("budget_untracked_fail", "fail",
                                      5.0)])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "budget: missing rows are report lines, not red, got rc=%r "
              "out=%r" % (rc, out))
        check("MISSING ctest-Debug budget_untracked_run" in out
              and "MISSING ctest-Debug budget_untracked_fail" in out,
              "budget: every unresolvable junit entry must surface in the "
              "missing set, got out=%r" % out)


def test_check_budget_scalar_scales_budget_not_ratchet():
    # D-1 scales the budget with the machine scalar; D-15's ratchet line
    # stays reference-based on purpose — D-4's opposite-direction argument:
    # a contention-inflated scalar must not make normal readings look
    # worth lowering the reference for.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_flat_entry", "run", 201.0)],
                                    extra=["--scalar", "2.0"])
        check(rc == 0 and "BUDGET-FAIL" not in out
              and "advisory: budget_flat_entry" not in out,
              "budget: at scalar 2 the same 201 s reading is inside its "
              "budget and above the reference-based line, got rc=%r "
              "out=%r" % (rc, out))
        rc, out = _run_check_budget(table, "ctest-Debug",
                                    [("budget_flat_entry", "run", 50.0)],
                                    extra=["--scalar", "2.0"])
        check(rc == 0 and "advisory: budget_flat_entry 建议下调参考" in out,
              "budget: the ratchet threshold must ignore the scalar, got "
              "rc=%r out=%r" % (rc, out))


# The 100.00 reference of the budget_ratchet_* rows in numeric form: the
# corridor readings below are computed from this and SAFETY_MULTIPLIER
# instead of hand-typed numbers, so they keep straddling the two lines
# even if the pinned constant moves.
RATCHET_REF_S = 100.0


def test_check_budget_ratchet_line_scalar_free():
    # D-15's reference-based ratchet and D-4's opposite-direction clause
    # (a contention-inflated scalar must not make normal readings look
    # worth lowering the reference for), pinned by a discriminating
    # corridor: mid sits between the reference line and what that line
    # would become if the scalar multiplied in, so a scalar-carrying
    # ratchet line fires an advisory here and must not. low sits under
    # both lines, so the advisory must still fire -- without it, turning
    # the ratchet off entirely would also read green.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        line = RATCHET_REF_S * SAFETY_MULTIPLIER / 2
        polluted = line * 2.0                      # scalar 2 multiplied in
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_ratchet_mid_entry", "run", (line + polluted) / 2),
             ("budget_ratchet_low_entry", "run", line / 2)],
            extra=["--scalar", "2.0"])
        check(rc == 0 and "BUDGET-FAIL" not in out,
              "budget: readings inside the corridor must not red, got "
              "rc=%r out=%r" % (rc, out))
        check("advisory: budget_ratchet_mid_entry" not in out,
              "budget: the ratchet line must not carry the machine "
              "scalar, got out=%r" % out)
        check("advisory: budget_ratchet_low_entry 建议下调参考" in out,
              "budget: the reading under both lines must still fire the "
              "advisory, got out=%r" % out)


def test_check_budget_ratchet_line_carries_work_ratio():
    # The ratchet line carries the row's own work ratio (that half is
    # workload, not machine). At work factor 2 the line sits at
    # ratio * SAFETY_MULTIPLIER / 2 times the reference: the ratio_entry
    # reading is under the ratio-carrying line and over the ratio-free
    # one, so its advisory must fire; the high_entry reading is over the
    # line and under a scalar-multiplied one, so it must stay silent.
    # Each pair of assertions alone turns this case red on one of the
    # two mutations (ratio dropped / scalar multiplied in).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        ratio_line = RATCHET_REF_S * 2.0 * SAFETY_MULTIPLIER / 2
        polluted = ratio_line * 2.0                # scalar 2 multiplied in
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_ratchet_ratio_entry", "run",
              (RATCHET_REF_S + ratio_line) / 2),
             ("budget_ratchet_high_entry", "run",
              (ratio_line + polluted) / 2)],
            extra=["--work-factor", "2.0", "--scalar", "2.0"])
        check(rc == 0 and "BUDGET-FAIL" not in out,
              "budget: readings inside the corridor must not red, got "
              "rc=%r out=%r" % (rc, out))
        check("advisory: budget_ratchet_ratio_entry 建议下调参考" in out,
              "budget: the work ratio must enter the ratchet line, got "
              "out=%r" % out)
        check("advisory: budget_ratchet_high_entry" not in out,
              "budget: the ratchet line must not carry the machine "
              "scalar at work factor 2, got out=%r" % out)


def test_check_budget_selfreport_missing_red():
    # D-8's self-report contract: the row prices 100 soak rounds and the
    # entry printed no rounds=<n> line on its captured stdout. Absence is
    # red rather than defaulted -- silently dropping the report would
    # otherwise read as a free speedup of the soak, which is exactly the
    # drift the contract exists to catch ("no report, red").
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_rounds_missing_entry", "run", 100.0)])
        check(rc == 1 and "GATE gate_time_check: FAIL" in out,
              "budget: a rounds-priced row without a self-report must "
              "go red, got rc=%r out=%r" % (rc, out))
        check("SELFREPORT budget_rounds_missing_entry: no rounds=<n> "
              "line" in out and "ref_work=100" in out,
              "budget: the finding must name the entry and the work its "
              "row prices, got out=%r" % out)


def test_check_budget_selfreport_tracks_reference_and_factor():
    # Both halves of the D-8 equation (self-report == ref_work x work
    # factor) pinned by a discriminating triple: at factor 1 the row's
    # own rounds pass; at factor 2 the doubled report passes while the
    # undoubled one goes red naming both numbers -- catching "the factor
    # was set but the soak loop never read it".
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_rounds_ok_entry", "run", 100.0, "rounds=100\n")])
        check(rc == 0 and "SELFREPORT" not in out,
              "budget: a matching self-report must pass, got rc=%r "
              "out=%r" % (rc, out))
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_rounds_ok_entry", "run", 200.0, "rounds=200\n")],
            extra=["--work-factor", "2.0"])
        check(rc == 0 and "SELFREPORT" not in out,
              "budget: at factor 2 the doubled self-report must pass, "
              "got rc=%r out=%r" % (rc, out))
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_rounds_ok_entry", "run", 200.0, "rounds=100\n")],
            extra=["--work-factor", "2.0"])
        check(rc == 1 and "SELFREPORT budget_rounds_ok_entry: "
              "rounds=100, expected 200 (ref_work=100 x work factor 2)"
              in out,
              "budget: at factor 2 an undoubled self-report must red "
              "with both numbers, got rc=%r out=%r" % (rc, out))


def test_check_budget_inline_exceeded_names_its_cause():
    # D-2's no-double-verdict on the fail-triage line: the qtest wrapper
    # already judged this entry over budget (BUDGET-EXCEEDED on its
    # captured stdout, exit 1), so ctest's fail row IS that verdict --
    # the listing says so instead of reading as an unexplained test
    # failure, and a plain failure without the marker keeps the bare
    # wording (test_check_budget_failed_row_informational_only).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, BUDGET_TABLE)
        rc, out = _run_check_budget(
            table, "ctest-Debug",
            [("budget_failed_entry", "fail", 5.0,
              "  BUDGET-EXCEEDED budget=200 actual=240\n")])
        check(rc == 0
              and "BUDGET budget_failed_entry FAILED actual=5.00 "
                  "(inline BUDGET-EXCEEDED)" in out,
              "budget: an inline over-budget verdict must own the fail "
              "listing, got rc=%r out=%r" % (rc, out))


def test_check_budget_coarse_rows_charging_rounds_refuse_pairing():
    # Cleanup six: a rounds self-report pairs only with the entry's OWN
    # row (D-8) -- a coarse row reads one measurement for a whole scope,
    # so one charging ref_work != 1 priced work the scope-granularity
    # skip used to pass silently: refuse that pairing by name (a plain
    # ref_work = 1 coarse row has no rounds axis to verify and passes).
    probe = [("budget_probe_entry", "run", 150.0, "rounds=100\n")]
    with tempfile.TemporaryDirectory() as tmp:
        for work, want in ((100, 1), (1, 0)):
            table = write_table(tmp, "ctest-Debug default 100.00 %d 常规 "
                                "ctest-junit-serial - coarse row\n" % work)
            rc, out = _run_check_budget(table, "ctest-Debug", probe)
            refused = "SELFREPORT budget_probe_entry" in out and "own row" in out
            check(rc == want and refused == (want == 1),
                  "budget: coarse ref_work=%d may refuse the self-report "
                  "only while it charges rounds, got rc=%r out=%r"
                  % (work, rc, out))


def run():
    test_check_budget_over_entry_row_red()
    test_check_budget_timeout_watchdog_wording_once()
    test_check_budget_run_and_fail_coexist_watchdog_wins()
    test_check_budget_failed_row_informational_only()
    test_check_budget_default_row_over_budget_advisory_only()
    test_check_budget_manifest_row_over_budget_advisory_only()
    test_check_budget_manifest_row_beats_default_row()
    test_check_budget_under_half_entry_row_advisory_only()
    test_check_budget_aggregate_ratchet_once_with_max_reading()
    test_check_budget_form_mismatch_red()
    test_check_budget_missing_rows_are_report_lines()
    test_check_budget_scalar_scales_budget_not_ratchet()
    test_check_budget_ratchet_line_scalar_free()
    test_check_budget_ratchet_line_carries_work_ratio()
    test_check_budget_selfreport_missing_red()
    test_check_budget_selfreport_tracks_reference_and_factor()
    test_check_budget_coarse_rows_charging_rounds_refuse_pairing()
    test_check_budget_inline_exceeded_names_its_cause()
