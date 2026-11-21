# gate_time_check_test.py -- self-test corpus for the reference-timing
# guards (tools/gate_time_check.py: guard one completeness, guard two
# orphans, guard seven constant band, and the guard-six member that
# --record must keep the four human columns). Stub trees and stub tables
# in the shape of real entry names keep the corpus hermetic: no third-party
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
import gate_time_check
from gate_time_check import (
    CONFIG_KEYS,
    SAFETY_MULTIPLIER,
    apply_record,
    caps,
    check_complete,
    check_constants,
    check_orphans,
    load_table,
    main as run_cli,
    parse_junit_rows,
    parse_measure_rows,
)

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def close(a, b):
    return abs(a - b) < 1e-6


# Stub entry universes, in the shape of real ctest entry names and e2e
# manifest row names.
CTEST_NAMES = ["alpha_probe_tests", "beta_probe_tests", "gamma_probe_tests",
               "delta_probe_tests", "epsilon_probe_tests",
               "zeta_probe_tests"]
PERF_NAMES = ["alpha_perf_compile", "alpha_perf_run"]
MANIFEST_NAMES = ["manifest_probe_a", "manifest_probe_b", "manifest_probe_c",
                  "manifest_probe_d", "manifest_probe_e", "manifest_probe_f"]
EXAMPLES_NAMES = ["example_probe_a", "example_probe_b"]


def base_table_text():
    """A fully consistent table: every derived entry has a row and every
    participating config carries exactly five sentinel marks."""
    lines = ["# stub reference table (gate_time_check selftest)"]
    for cfg in ("ctest-Debug", "ctest-IDE", "ctest-ASan"):
        for i, name in enumerate(CTEST_NAMES):
            lines.append("%s %s 10.00 1 常规 ctest-junit-serial %s stub"
                         % (cfg, name, "yes" if i < 5 else "-"))
    for name in PERF_NAMES:
        lines.append("ctest-Release %s 10.00 1 性能 ctest-junit-serial - "
                     "perf wall-clock stub" % name)
    for name in CTEST_NAMES[:5]:
        lines.append("ctest-Release %s 10.00 1 常规 ctest-junit-serial yes "
                     "release sentinel stub" % name)
    for cfg in ("e2e-Debug", "e2e-ASan", "e2e-Release"):
        lines.append("%s default 10.00 1 常规 e2e-serial - stub default"
                     % cfg)
        for name in MANIFEST_NAMES[:5]:
            lines.append("%s %s 10.00 1 常规 e2e-serial yes e2e sentinel "
                         "stub" % (cfg, name))
    lines.append("e2e-Release examples_manifest.txt 10.00 1 演示验收 "
                 "e2e-serial - examples scope stub")
    return "\n".join(lines) + "\n"


def base_enums():
    return {"ctest-Debug": set(CTEST_NAMES),
            "ctest-IDE": set(CTEST_NAMES),
            "ctest-ASan": set(CTEST_NAMES)}


def base_manifests():
    return {"manifest.txt": set(MANIFEST_NAMES),
            "examples_manifest.txt": set(EXAMPLES_NAMES)}


def base_perf_on():
    return set(PERF_NAMES)


def _write_table(tmp, text, name="reference.txt"):
    path = Path(tmp) / name
    path.write_text(text, encoding="utf-8")
    return path


def _drop_line(text, prefix):
    lines = text.splitlines()
    kept = [ln for ln in lines if not ln.startswith(prefix)]
    assert len(kept) < len(lines), "stub line %r not found" % prefix
    return "\n".join(kept) + "\n"


def test_complete_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = _write_table(tmp, base_table_text())
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_on())
        check(ok, "complete: base stub table must pass, findings=%r"
              % findings)


def test_complete_missing_row_fails():
    with tempfile.TemporaryDirectory() as tmp:
        text = _drop_line(base_table_text(),
                          "ctest-Debug zeta_probe_tests ")
        path = _write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_on())
        check(not ok, "complete: a dropped ctest-Debug row must fail")
        check(any(f.startswith("MISSING ctest-Debug zeta_probe_tests")
                  for f in findings),
              "complete: the missing entry must be named, findings=%r"
              % findings)


def test_complete_extra_row_fails():
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text() + ("ctest-Debug gone_probe_tests 10.00 1 "
                                    "常规 ctest-junit-serial - stub extra\n")
        path = _write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_on())
        check(not ok, "complete: a row for an unenumerated entry must fail")
        check(any(f.startswith("EXTRA ctest-Debug gone_probe_tests")
                  for f in findings),
              "complete: the extra entry must be named, findings=%r"
              % findings)


def test_complete_sentinels_cleared_fails():
    # Degenerate state: the whole sentinel column emptied shrinks the
    # ctest-Release union and leaves every calibration config with no
    # sentinel input -- guard one must catch it, not pass vacuously.
    with tempfile.TemporaryDirectory() as tmp:
        text = (base_table_text()
                .replace("ctest-junit-serial yes ", "ctest-junit-serial - ")
                .replace("e2e-serial yes ", "e2e-serial - "))
        path = _write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_on())
        check(not ok, "complete: an emptied sentinel column must fail")
        for cfg in ("ctest-Debug", "ctest-IDE", "ctest-ASan",
                    "ctest-Release", "e2e-Debug", "e2e-ASan",
                    "e2e-Release"):
            check(any(f.startswith("SENTINELS %s:" % cfg) for f in findings),
                  "complete: sentinel finding expected for %s, findings=%r"
                  % (cfg, findings))


def test_complete_too_many_sentinels_fails():
    with tempfile.TemporaryDirectory() as tmp:
        extra = "".join(
            "ctest-Debug eta_probe_%d 10.00 1 常规 ctest-junit-serial yes "
            "stub\n" % i for i in range(1, 5))
        path = _write_table(tmp, base_table_text() + extra)
        enums = base_enums()
        enums["ctest-Debug"] |= {"eta_probe_%d" % i for i in range(1, 5)}
        ok, findings = check_complete(load_table(path), enums,
                                      base_manifests(), base_perf_on())
        check(not ok, "complete: nine sentinel marks in one config must fail")
        check(any(f.startswith("SENTINELS ctest-Debug: 9")
                  for f in findings),
              "complete: the over-marked config must be named, findings=%r"
              % findings)


def test_orphans_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = _write_table(tmp, base_table_text())
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_on(), base_manifests())
        check(ok, "orphans: base stub table must pass, findings=%r"
              % findings)


def test_orphan_renamed_ctest_entry_fails():
    # Guard two: a renamed ctest entry leaves its reference rows pointing
    # at a name no tree enumerates any more.
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text().replace("alpha_probe_tests",
                                         "alpha_probe_tests_renamed")
        path = _write_table(tmp, text)
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_on(), base_manifests())
        check(not ok, "orphans: a renamed entry must fail")
        orphans = [f for f in findings
                   if "alpha_probe_tests_renamed" in f]
        check(len(orphans) >= 3,
              "orphans: every stale config row must be reported, findings=%r"
              % findings)


def test_orphan_renamed_e2e_entry_fails():
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text().replace("manifest_probe_a",
                                         "manifest_probe_a_gone")
        path = _write_table(tmp, text)
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_on(), base_manifests())
        check(not ok, "orphans: a renamed manifest row must fail")
        check(any("manifest_probe_a_gone" in f for f in findings),
              "orphans: the renamed manifest entry must be named, findings=%r"
              % findings)


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
        path = _write_table(tmp, (
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
        path = _write_table(tmp, (
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


CAPS_TABLE = (
    "# stub caps table\n"
    "ctest-Debug caps_over_floor 100.00 1 常规 ctest-junit-serial - stub\n"
    "ctest-Debug caps_small_entry 5.00 1 常规 ctest-junit-serial - stub\n"
    "ctest-Debug caps_tiny_entry 1.00 1 常规 ctest-junit-serial - stub\n")


def test_caps_work_factor_two_doubles_cap_and_budget():
    # D-1: the work factor must scale the watchdog cap together with the
    # budget, or factor 2 would let the watchdog kill legitimate long runs.
    with tempfile.TemporaryDirectory() as tmp:
        path = _write_table(tmp, CAPS_TABLE)
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
        path = _write_table(tmp, CAPS_TABLE)
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
        path = _write_table(tmp, CAPS_TABLE)
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


def test_constants_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = _write_table(tmp, base_table_text())
        ok, findings = check_constants(load_table(path))
        check(ok, "constants: the base stub table must pass, findings=%r"
              % findings)


def test_constants_sentinel_reference_above_min_cap_fails():
    # Guard seven: a sentinel whose reference exceeds MIN_CAP_S would be
    # killed by its own budget floor during calibration.
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text() + ("ctest-Debug slow_sentinel_tests "
                                    "500.00 1 常规 ctest-junit-serial yes "
                                    "over-long sentinel stub\n")
        path = _write_table(tmp, text)
        ok, findings = check_constants(load_table(path))
        check(not ok, "constants: an over-long sentinel reference must fail")
        check(any("MIN_CAP_S" in f for f in findings),
              "constants: the MIN_CAP_S clause must be reported, "
              "findings=%r" % findings)


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
    "e2e-Debug default 10.00 1 常规 e2e-serial - coarse scope stub\n"
    "e2e-ASan manifest.txt 10.00 1 常规 e2e-serial - manifest scope stub\n")


def _junit_xml(cases):
    """Build a junit document in the CMake 3.25.2 shape the checker must
    parse (status="run" completed, status="fail" for both Failed and
    Timeout — no attribute tells them apart)."""
    body = "".join('<testcase name="%s" status="%s" time="%.4f"/>'
                   % (name, status, secs) for name, status, secs in cases)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, (
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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
        table = _write_table(tmp, BUDGET_TABLE)
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


def _stub_ctest_sampler(readings, calls=None):
    """Replace the live ctest sampling round with a canned reading map for
    one calibrate CLI call (the stub runner of guard four: no ctest run).
    Returns the captured stdout/return-code pair."""
    def _patched(config, tree, names):
        if calls is not None:
            calls.append(config)
        return readings
    return _patched


def test_calibrate_median_and_scale_line():
    # Guard four: the machine scalar is the MEDIAN of per-sentinel
    # measured/reference ratios. Base-table sentinels all carry ref 10.00,
    # so the stub readings below sit at ratios 1/2/3/4/100 -- a mean would
    # report 22, the median reports 3.
    with tempfile.TemporaryDirectory() as tmp:
        table = _write_table(tmp, base_table_text())
        readings = {"alpha_probe_tests": (10.0, "ctest-junit-serial"),
                    "beta_probe_tests": (20.0, "ctest-junit-serial"),
                    "gamma_probe_tests": (30.0, "ctest-junit-serial"),
                    "delta_probe_tests": (40.0, "ctest-junit-serial"),
                    "epsilon_probe_tests": (1000.0, "ctest-junit-serial")}
        orig = gate_time_check._sample_ctest
        gate_time_check._sample_ctest = _stub_ctest_sampler(readings)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                              str(Path(tmp) / "tree"), "--table", str(table)])
        finally:
            gate_time_check._sample_ctest = orig
        out = buf.getvalue()
        check(rc == 0
              and "SCALE ctest-Debug scale=3.000 band=[0.25,4.0]" in out
              and "GATE gate_time_check: PASS" in out,
              "calibrate: the median ratio must be reported on the SCALE "
              "detail line, got rc=%r out=%r" % (rc, out))


def test_calibrate_out_of_band_red_with_candidates():
    # Guard four red side: an out-of-band scale names the config whose
    # reference needs re-recording and prints this round's measured rows
    # in table shape (the candidate rows a human re-records from).
    for label, secs in (("above", 80.0), ("below", 1.0)):
        with tempfile.TemporaryDirectory() as tmp:
            table = _write_table(tmp, base_table_text())
            readings = {name: (secs, "ctest-junit-serial")
                        for name in CTEST_NAMES[:5]}
            orig = gate_time_check._sample_ctest
            gate_time_check._sample_ctest = _stub_ctest_sampler(readings)
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                                  str(Path(tmp) / "tree"),
                                  "--table", str(table)])
            finally:
                gate_time_check._sample_ctest = orig
            out = buf.getvalue()
            check(rc == 1 and "GATE gate_time_check: FAIL" in out,
                  "calibrate %s band: must exit red, got rc=%r out=%r"
                  % (label, rc, out))
            check("BAND ctest-Debug" in out,
                  "calibrate %s band: the config to re-record must be "
                  "named, got out=%r" % (label, out))
            check("CANDIDATE ctest-Debug alpha_probe_tests %.2f 1 常规 "
                  "ctest-junit-serial yes stub" % secs in out,
                  "calibrate %s band: this round's measured candidate rows "
                  "must be printed, got out=%r" % (label, out))
            # The SCALE line stays printed on the red path too (the detail
            # line is what a human reads the band decision from).
            check("SCALE ctest-Debug scale=" in out,
                  "calibrate %s band: the SCALE line must still print, got "
                  "out=%r" % (label, out))


def test_calibrate_fewer_than_five_sentinels_red_without_sampling():
    # Guard four admission: a participating config with fewer than
    # SENTINELS_PER_CONFIG[0] sentinel rows cannot estimate a median, and
    # the round must not sample at all (a real ctest run would burn
    # minutes before the shortfall mattered).
    with tempfile.TemporaryDirectory() as tmp:
        table = _write_table(tmp, _drop_line(base_table_text(),
                                             "ctest-Debug epsilon_probe_tests "))
        calls = []
        orig = gate_time_check._sample_ctest
        gate_time_check._sample_ctest = _stub_ctest_sampler({}, calls)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                              str(Path(tmp) / "tree"), "--table", str(table)])
        finally:
            gate_time_check._sample_ctest = orig
        out = buf.getvalue()
        check(rc == 1 and "SENTINELS ctest-Debug: 4" in out,
              "calibrate: four sentinel rows must go red naming the count, "
              "got rc=%r out=%r" % (rc, out))
        check(calls == [], "calibrate: the shortfall path must not start a "
              "sampling round, got calls=%r" % (calls,))


def test_calibrate_flag_config_mismatch_red():
    # The source flag follows the config family (ctest trees enumerate and
    # run through ctest, e2e configs through the runner's tools): a crossed
    # pairing is a caller mistake and must fail with the flag it wants, not
    # an enumeration crash later.
    with tempfile.TemporaryDirectory() as tmp:
        table = _write_table(tmp, base_table_text())
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["calibrate", "e2e-Debug", "--tree",
                          str(Path(tmp) / "tree"), "--table", str(table)])
        out = buf.getvalue()
        check(rc == 1 and "--tools" in out,
              "calibrate: an e2e config with --tree must name --tools as "
              "the wanted flag, got rc=%r out=%r" % (rc, out))


def main():
    test_complete_ok_on_base_table()
    test_complete_missing_row_fails()
    test_complete_extra_row_fails()
    test_complete_sentinels_cleared_fails()
    test_complete_too_many_sentinels_fails()
    test_orphans_ok_on_base_table()
    test_orphan_renamed_ctest_entry_fails()
    test_orphan_renamed_e2e_entry_fails()
    test_record_preserves_human_columns()
    test_record_measure_group_max_and_default_skip()
    test_record_junit_group_max_never_sum()
    test_record_junit_keeps_only_run_status_rows()
    test_caps_work_factor_two_doubles_cap_and_budget()
    test_caps_floor_entry_watchdog_pinned_at_120()
    test_caps_clamp_lower_bound_does_not_invert()
    test_constants_ok_on_base_table()
    test_constants_sentinel_reference_above_min_cap_fails()
    test_cli_record_and_caps_verdicts()
    test_cli_caps_unknown_config_names_the_config_keys()
    test_calibrate_median_and_scale_line()
    test_calibrate_out_of_band_red_with_candidates()
    test_calibrate_fewer_than_five_sentinels_red_without_sampling()
    test_calibrate_flag_config_mismatch_red()
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
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE gate_time_check_selftest: FAIL (%d)" % len(failures))
        return 1
    print("GATE gate_time_check_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
