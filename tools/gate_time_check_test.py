# gate_time_check_test.py -- self-test corpus entry for the reference-timing
# guards (tools/gate_time_check.py and its gate_time_table /
# gate_time_guards / gate_time_measure / gate_time_timeouts modules:
# guard one completeness, guard two orphans, guard seven constant band,
# the guard-six member that
# --record must keep the three human columns, and the emit-cmake subcommand
# that derives the configure-time TIMEOUT caps). The case groups live in
# companion modules -- gate_time_table_test / gate_time_measure_test /
# gate_time_guards_test / gate_time_budget_test / gate_time_timeouts_test
# / gate_time_schedule_test / gate_time_perf_test /
# gate_time_carrier_test -- whose run() this file's main() calls
# first; what stays here are the
# CLI-facing cases (record, caps, emit-cmake) and the verdict line. Stub
# trees and stub tables in
# the shape of real entry names keep the corpus hermetic: no third-party
# imports, no ctest run, no network. Run:
#   python gate_time_check_test.py
# Verdict line: "GATE gate_time_check_selftest: PASS" (or FAIL), exit code
# follows.

import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_budget_test
import gate_time_carrier_test
import gate_time_guards_test
import gate_time_measure_test
import gate_time_perf_test
import gate_time_schedule_test
import gate_time_table_test
import gate_time_timeouts_test
from gate_time_check import main as run_cli
from gate_time_table import CONFIG_KEYS, load_table
from gate_time_testutil import CAPS_TABLE, check, close, failures, write_table


def test_cli_record_and_caps_verdicts():
    # The CLI wiring itself: record must report counts and pass, and the
    # caps subcommand must print its line and pass (verdict glyph per
    # subcommand).
    with tempfile.TemporaryDirectory() as tmp:
        table = Path(tmp) / "reference.txt"
        table.write_text(CAPS_TABLE, encoding="utf-8")
        meas = Path(tmp) / "measures.txt"
        meas.write_text("MEASURE caps_tiny_entry 3.00 "
                        "form=ctest-junit-serial\n", encoding="utf-8")
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["record", "--config", "ctest-Debug",
                       "--from-measure", str(meas), "--table", str(table)])
        out = buf.getvalue()
        check(rc == 0 and "RECORD ctest-Debug updated=1 added=0" in out
              and "GATE gate_time_check: PASS" in out,
              "cli: record must report counts and pass, got rc=%r out=%r"
              % (rc, out))
        tiny = [r for r in load_table(table)
                if r.entry == "caps_tiny_entry"][0]
        check(close(tiny.ref_s, 3.0),
              "cli: record must rewrite the table file, got %r" % (tiny,))
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["caps", "ctest-Debug", "caps_over_floor",
                       "--table", str(table)])
        out = buf.getvalue()
        check(rc == 0 and "budget_s=200.00 watchdog_s=1000.00" in out
              and "GATE gate_time_check: PASS" in out,
              "cli: caps must print the caps line and pass, got rc=%r "
              "out=%r" % (rc, out))


def test_cli_caps_unknown_config_names_the_config_keys():
    # A mistyped config key must be named against the legal seven-word
    # vocabulary: the missing-row wording would otherwise steer the user
    # toward "add a default row" as the wrong remedy. The verdict still
    # goes through the shared FAIL glyph.
    with tempfile.TemporaryDirectory() as tmp:
        table = Path(tmp) / "reference.txt"
        table.write_text(CAPS_TABLE, encoding="utf-8")
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["caps", "ctest-Todo", "caps_tiny_entry",
                          "--table", str(table)])
        out = buf.getvalue()
        check(rc == 1, "caps: an unknown config key must exit 1, got "
              "rc=%r out=%r" % (rc, out))
        check("GATE gate_time_check: FAIL" in out,
              "caps: the verdict glyph must stay the shared one, got "
              "out=%r" % out)
        for cfg in CONFIG_KEYS:
            check(cfg in out, "caps: the unknown-config failure must "
                  "list the legal config keys, %s missing from out=%r"
                  % (cfg, out))


EMIT_TABLE = (
    "# stub emit table\n"
    "ctest-Debug emit_over_floor 100.00 1 ctest-junit-serial - stub\n"
    "ctest-Debug emit_floor_entry 5.00 1 ctest-junit-serial - stub\n"
    "ctest-Debug emit.dot-dash-name 12.50 1 ctest-junit-serial - stub\n"
    "ctest-Release emit_release_only 200.00 1 ctest-junit-serial - stub\n"
    "ctest-Debug emit_both_configs 100.00 1 ctest-junit-serial - stub\n"
    "ctest-Release emit_both_configs 300.00 1 ctest-junit-serial - stub\n")

EMIT_KEYS = ["--config", "ctest-Debug", "--config", "ctest-Release"]


def _emit_run(tmp, table, extra=EMIT_KEYS):
    # Shared CLI probe: returns (rc, stdout, generated-path). The generated
    # file is the deliverable the CMake configure step includes, so every
    # emit-cmake check reads it back from --out.
    out = Path(tmp) / "thresholds.cmake"
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            rc = run_cli(["emit-cmake", "--table", str(table),
                          "--out", str(out)] + list(extra))
    except SystemExit as exc:
        # argparse errors (unknown subcommand, missing --out) exit outside
        # main's error handler; record the code instead of aborting the
        # corpus.
        rc = exc.code
    return rc, buf.getvalue(), out


def _emit_vars(path):
    # Parse generated lines into {variable name: float cap}; a line whose
    # shape is wrong raises, which is how the syntax pin reports. A missing
    # file yields {} so the rc/shape checks report instead of a traceback.
    vals = {}
    if not path.exists():
        return vals
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("set("):
            continue
        if line.startswith("set(GATE_RESOURCE_"):
            # The claim axis (D-11) belongs to _emit_resources; an
            # unknown set() shape still trips the assertion below.
            continue
        assert line.endswith(")"), line
        assert line.startswith("set(GATE_TIMEOUT_"), line
        var, value = line[len("set("):-1].split(" ")
        vals[var] = float(value)
    return vals


def test_cli_emit_cmake_writes_sanitized_variables():
    # The generated file must be valid CMake: a header comment, then one
    # set(GATE_TIMEOUT_<var> <value>) line per entry with a row under any
    # requested key. Entry-name punctuation sanitizes to '_' exactly as
    # the add_test wrapper sanitizes when it looks the variable up.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(tmp, table)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "emit-cmake: the happy path must pass, got rc=%r out=%r"
              % (rc, out))
        text = (path.read_text(encoding="utf-8") if path.exists() else "")
        head = text.splitlines()[:1]
        check(head and head[0].startswith("#"),
              "emit-cmake: the file must open with a header comment, "
              "got %r" % head)
        set_lines = [l for l in text.splitlines() if l.startswith("set(")]
        check(len(set_lines) == 5,
              "emit-cmake: one set() line per row-holding entry expected, "
              "got %r" % set_lines)
        for line in set_lines:
            check(line.startswith("set(GATE_TIMEOUT_") and line.endswith(")")
                  and len(line[len("set("):-1].split(" ")) == 2,
                  "emit-cmake: line must read set(GATE_TIMEOUT_<var> "
                  "<value>), got %r" % line)
        vals = _emit_vars(path)
        check(vals.get("GATE_TIMEOUT_emit_over_floor") == 1000.0,
              "emit-cmake: a 100 s reference must cap at 1000.0, got "
              "%r" % vals)
        check(vals.get("GATE_TIMEOUT_emit_floor_entry") == 120.0,
              "emit-cmake: a 5 s reference must sit on the MIN_WATCHDOG_S "
              "floor, got %r" % vals)
        check(vals.get("GATE_TIMEOUT_emit_dot_dash_name") == 125.0,
              "emit-cmake: the sanitized name must carry the 12.5 s "
              "reference cap, got %r" % vals)
        check(vals.get("GATE_TIMEOUT_emit_release_only") == 2000.0,
              "emit-cmake: a Release-row entry must appear when "
              "ctest-Release is requested, got %r" % vals)
        check("emit.dot-dash" not in text,
              "emit-cmake: entry-name punctuation must never reach a "
              "variable name")


EMIT_CLAIMS = ("# gate_tiers.txt shape: tier lines are ignored, claim "
               "lines feed RESOURCE_LOCK\n"
               "tier T1-debug-e2e selector .\n"
               "claim nide-gui mainwindow_tests emit_over_floor "
               "emit.dot-dash-name\n"
               "claim display-server mainwindow_tests\n")


def _emit_resources(path):
    # Parse the generated resource lines into {variable: "name1 name2"};
    # a line whose shape is wrong raises, same reporting style as
    # _emit_vars.
    vals = {}
    if not path.exists():
        return vals
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("set(GATE_RESOURCE_"):
            continue
        assert line.endswith(")"), line
        var, names = line[len("set("):-1].split(" ", 1)
        vals[var] = names
    return vals


def test_cli_emit_cmake_claims_emit_resource_lock_variables():
    # D-11: a --claims roster (the tools/gate_tiers.txt claim lines) adds
    # one set(GATE_RESOURCE_<sanitized entry> <claim names...>) line per
    # claimed entry, the variable the add_test wrapper turns into the
    # ctest RESOURCE_LOCK property. Roster order is kept and an entry in
    # two claims names both locks; claim entries get the variable even
    # when the table holds no timing row for them (a claimed entry with
    # no row still must not run beside another window launcher).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        claims = Path(tmp) / "claims.txt"
        claims.write_text(EMIT_CLAIMS, encoding="utf-8")
        rc, out, path = _emit_run(tmp, table,
                                  EMIT_KEYS + ["--claims", str(claims)])
        check(rc == 0, "emit-cmake claims: the happy path must pass, "
              "got rc=%r out=%r" % (rc, out))
        res = _emit_resources(path)
        check(res.get("GATE_RESOURCE_mainwindow_tests")
              == "nide-gui display-server",
              "emit-cmake claims: an entry in two claims must name both "
              "locks in roster order, got %r" % res)
        check(res.get("GATE_RESOURCE_nide_gui") is None,
              "emit-cmake claims: claim names never become variables "
              "themselves, got %r" % res)
        check(res.get("GATE_RESOURCE_emit_over_floor") == "nide-gui",
              "emit-cmake claims: a table row entry must also carry its "
              "lock, got %r" % res)
        check(res.get("GATE_RESOURCE_emit_dot_dash_name") == "nide-gui",
              "emit-cmake claims: entry-name punctuation sanitizes to '_' "
              "in the resource variable too, got %r" % res)
        text = (path.read_text(encoding="utf-8") if path.exists()
                else "")
        check("emit.dot-dash" not in text,
              "emit-cmake claims: punctuation must never reach a "
              "variable name")
        # The TIMEOUT caps stay untouched by the claims axis (the same
        # caps the claims-less run wrote).
        vals = _emit_vars(path)
        check(vals.get("GATE_TIMEOUT_emit_over_floor") == 1000.0,
              "emit-cmake claims: the TIMEOUT caps must be unaffected, "
              "got %r" % vals)


def test_cli_emit_cmake_claims_bad_roster_is_rejected():
    # A malformed claim roster is a named refusal (ERROR + FAIL(error)),
    # never a silently lock-less generated file: the tier_coverage_check
    # guard reads the same shape, so both must reject the same lines.
    for label, text in (
            ("no entries", "claim nide-gui\n"),
            ("bad resource name", "claim bad/name e1\n"),
            ("no resource name", "claim\n")):
        with tempfile.TemporaryDirectory() as tmp:
            table = write_table(tmp, EMIT_TABLE)
            claims = Path(tmp) / "claims.txt"
            claims.write_text(text, encoding="utf-8")
            rc, out, path = _emit_run(tmp, table,
                                      EMIT_KEYS + ["--claims", str(claims)])
            check(rc == 1 and "ERROR" in out
                  and "GATE gate_time_check: FAIL (error)" in out,
                  "emit-cmake claims: a claim roster %s must be a named "
                  "refusal, got rc=%r out=%r" % (label, rc, out))
            rc2, _, _ = _emit_run(tmp, table, EMIT_KEYS)
            check(rc2 == 0, "emit-cmake claims: the same run without "
                  "--claims must stay green, got rc=%r" % rc2)


def test_cli_emit_cmake_takes_the_max_watchdog_across_configs():
    # D-2: a tree configured under several keys (e.g. Debug ∪ Release with
    # NLANG_PERF_TESTS=ON) gets ONE TIMEOUT per entry, the largest cap over
    # the keys carrying a row, so one ctest -C run never carries the cap
    # tuned for a different tree.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(tmp, table, ["--config", "ctest-Debug"])
        check(rc == 0 and _emit_vars(path).get(
            "GATE_TIMEOUT_emit_both_configs") == 1000.0,
              "emit-cmake: the Debug-only key set must use the Debug row, "
              "got rc=%r out=%r" % (rc, out))
        rc, out, path = _emit_run(tmp, table)
        check(rc == 0 and _emit_vars(path).get(
            "GATE_TIMEOUT_emit_both_configs") == 3000.0,
              "emit-cmake: the Debug-plus-Release key set must take the "
              "larger Release cap, got rc=%r out=%r" % (rc, out))


def test_cli_emit_cmake_work_factor_doubles_caps():
    # D-1: the configure-time work factor scales the watchdog cap together
    # with the budget (caps() is the single site); the sub-floor reference
    # stays pinned at MIN_WATCHDOG_S.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(
            tmp, table, EMIT_KEYS + ["--work-factor", "2.0"])
        vals = _emit_vars(path) if rc == 0 else {}
        check(rc == 0, "emit-cmake: factor 2 must stay green, got rc=%r "
              "out=%r" % (rc, out))
        check(vals.get("GATE_TIMEOUT_emit_over_floor") == 2000.0,
              "emit-cmake: factor 2 must double the 100 s cap, got "
              "%r" % vals)
        check(vals.get("GATE_TIMEOUT_emit_dot_dash_name") == 250.0,
              "emit-cmake: factor 2 must double the 12.5 s cap (above the "
              "12 s floor), got %r" % vals)
        check(vals.get("GATE_TIMEOUT_emit_floor_entry") == 120.0,
              "emit-cmake: the sub-12 s reference must stay floored even "
              "at factor 2, got %r" % vals)


def test_cli_emit_cmake_work_factor_below_one_is_rejected():
    # D-8: a factor below 1 must fail the configure (rc != 0 feeds the
    # CMake FATAL_ERROR), not silently shrink the caps below the committed
    # table -- and must not leave a half-written generated file behind.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(
            tmp, table, EMIT_KEYS + ["--work-factor", "0.5"])
        check(rc == 1 and "GATE gate_time_check: FAIL" in out,
              "emit-cmake: factor 0.5 must exit 1 with the shared FAIL "
              "glyph, got rc=%r out=%r" % (rc, out))
        check("work factor" in out.lower(),
              "emit-cmake: the rejection must name the work factor, got "
              "out=%r" % out)
        check(not path.exists(),
              "emit-cmake: the rejected configure must not write the "
              "generated file")


def test_cli_emit_cmake_omitted_work_factor_equals_one():
    # The CMake side omits --work-factor entirely when
    # NLANG_TEST_WORK_FACTOR is unset (an empty value would poison the
    # argument list), so the omitted-factor run must equal the explicit
    # 1.0 run byte for byte.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc1, out1, path1 = _emit_run(tmp, table)
        rc2, out2, path2 = _emit_run(
            tmp, table, EMIT_KEYS + ["--work-factor", "1.0"])
        b1 = path1.read_bytes() if path1.exists() else b""
        b2 = path2.read_bytes() if path2.exists() else b""
        check(rc1 == 0 and rc2 == 0,
              "emit-cmake: both factor spellings must pass, got rc=%r/%r"
              % (rc1, rc2))
        check(bool(b1) and b1 == b2,
              "emit-cmake: the omitted work factor must behave exactly "
              "like the explicit 1.0")


def test_cli_emit_cmake_rowless_entries_get_no_variable():
    # An entry without a row under any requested key must not appear in the
    # generated file at all: the wrapper then leaves it unmanaged and the
    # leg's ctest --timeout fallback owns it (the ownership split the
    # comments in gate_tier.sh and the qtest helper state).
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(tmp, table, ["--config", "ctest-Debug"])
        check(rc == 0, "emit-cmake: a Debug-only run must pass, got rc=%r "
              "out=%r" % (rc, out))
        vals = _emit_vars(path)
        check(sorted(vals) == sorted([
            "GATE_TIMEOUT_emit_both_configs",
            "GATE_TIMEOUT_emit_dot_dash_name",
            "GATE_TIMEOUT_emit_floor_entry",
            "GATE_TIMEOUT_emit_over_floor"]),
              "emit-cmake: exactly the entries with a Debug row must get "
              "variables, got %r" % sorted(vals))
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        check("GATE_TIMEOUT_emit_release_only" not in text,
              "emit-cmake: the Release-row entry must be absent from a "
              "Debug-only run")


def test_cli_emit_cmake_output_never_mentions_machine_scale():
    # D-4: the machine scale reaches test processes only through the leg
    # environment, so nothing scalar-dependent (scale, budget) may be
    # baked at configure time -- the generated file and the status line
    # must stay scale-free by construction.
    with tempfile.TemporaryDirectory() as tmp:
        table = write_table(tmp, EMIT_TABLE)
        rc, out, path = _emit_run(tmp, table)
        text = path.read_text(encoding="utf-8") if rc == 0 else ""
        check(rc == 0, "emit-cmake: the run must pass, got rc=%r out=%r"
              % (rc, out))
        check("NLANG_GATE_MACHINE_SCALE" not in text
              and "NLANG_GATE_MACHINE_SCALE" not in out,
              "emit-cmake: no machine-scale note may survive in the "
              "generated file or stdout")


def main():
    gate_time_table_test.run()
    gate_time_measure_test.run()
    gate_time_guards_test.run()
    gate_time_budget_test.run()
    gate_time_timeouts_test.run()
    gate_time_schedule_test.run()
    gate_time_perf_test.run()
    gate_time_carrier_test.run()
    test_cli_record_and_caps_verdicts()
    test_cli_caps_unknown_config_names_the_config_keys()
    test_cli_emit_cmake_writes_sanitized_variables()
    test_cli_emit_cmake_claims_emit_resource_lock_variables()
    test_cli_emit_cmake_claims_bad_roster_is_rejected()
    test_cli_emit_cmake_takes_the_max_watchdog_across_configs()
    test_cli_emit_cmake_work_factor_doubles_caps()
    test_cli_emit_cmake_work_factor_below_one_is_rejected()
    test_cli_emit_cmake_omitted_work_factor_equals_one()
    test_cli_emit_cmake_rowless_entries_get_no_variable()
    test_cli_emit_cmake_output_never_mentions_machine_scale()
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE gate_time_check_selftest: FAIL (%d)" % len(failures))
        return 1
    print("GATE gate_time_check_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
