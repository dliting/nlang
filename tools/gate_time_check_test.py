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
from gate_time_check import (
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
              time="250.5"/>
    <testcase name="fresh_missing_entry" time="12.0"/>
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
    test_caps_work_factor_two_doubles_cap_and_budget()
    test_caps_floor_entry_watchdog_pinned_at_120()
    test_caps_clamp_lower_bound_does_not_invert()
    test_constants_ok_on_base_table()
    test_constants_sentinel_reference_above_min_cap_fails()
    test_cli_record_and_caps_verdicts()
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE gate_time_check_selftest: FAIL (%d)" % len(failures))
        return 1
    print("GATE gate_time_check_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
