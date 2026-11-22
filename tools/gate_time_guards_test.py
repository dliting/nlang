# gate_time_guards_test.py -- guard verdict cases for the guards module
# (gate_time_guards.py): guard one coverage and sentinel counts, guard
# two orphans, guard seven constant band, and guard four's calibrate CLI
# rounds with the live ctest sampling stubbed out (no real run); guard
# five's registered-cap cases live in the sibling
# gate_time_timeouts_test.py. The stub entry universes are shared by
# these cases only, so they stay here. Entry point:
# gate_time_check_test.py (its main() calls run()). Stdlib only; run
# with -I.

import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_check
from gate_time_check import main as run_cli
from gate_time_guards import check_complete, check_constants, check_orphans
from gate_time_table import load_table
from gate_time_testutil import check, drop_line, write_table

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
            lines.append("%s %s 10.00 1 ctest-junit-serial %s stub"
                         % (cfg, name, "yes" if i < 5 else "-"))
    for name in PERF_NAMES:
        lines.append("ctest-Release %s 10.00 1 ctest-junit-serial - "
                     "perf wall-clock stub" % name)
    for name in CTEST_NAMES[:5]:
        lines.append("ctest-Release %s 10.00 1 ctest-junit-serial yes "
                     "release sentinel stub" % name)
    for cfg in ("e2e-Debug", "e2e-ASan", "e2e-Release"):
        lines.append("%s default 10.00 1 e2e-serial - stub default"
                     % cfg)
        for name in MANIFEST_NAMES[:5]:
            lines.append("%s %s 10.00 1 e2e-serial yes e2e sentinel "
                         "stub" % (cfg, name))
    lines.append("e2e-Release examples_manifest.txt 10.00 1 e2e-serial "
                 "- examples scope stub")
    return "\n".join(lines) + "\n"


def base_enums():
    return {"ctest-Debug": set(CTEST_NAMES),
            "ctest-IDE": set(CTEST_NAMES),
            "ctest-ASan": set(CTEST_NAMES)}


def base_manifests():
    return {"manifest.txt": set(MANIFEST_NAMES),
            "examples_manifest.txt": set(EXAMPLES_NAMES)}


def base_perf_names():
    return set(PERF_NAMES)


def test_complete_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, base_table_text())
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_names())
        check(ok, "complete: base stub table must pass, findings=%r"
              % findings)


def test_complete_missing_row_fails():
    with tempfile.TemporaryDirectory() as tmp:
        text = drop_line(base_table_text(),
                         "ctest-Debug zeta_probe_tests ")
        path = write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_names())
        check(not ok, "complete: a dropped ctest-Debug row must fail")
        check(any(f.startswith("MISSING ctest-Debug zeta_probe_tests")
                  for f in findings),
              "complete: the missing entry must be named, findings=%r"
              % findings)


def test_complete_extra_row_fails():
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text() + ("ctest-Debug gone_probe_tests 10.00 1 "
                                    "ctest-junit-serial - stub extra\n")
        path = write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_names())
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
        path = write_table(tmp, text)
        ok, findings = check_complete(load_table(path), base_enums(),
                                      base_manifests(), base_perf_names())
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
            "ctest-Debug eta_probe_%d 10.00 1 ctest-junit-serial yes "
            "stub\n" % i for i in range(1, 5))
        path = write_table(tmp, base_table_text() + extra)
        enums = base_enums()
        enums["ctest-Debug"] |= {"eta_probe_%d" % i for i in range(1, 5)}
        ok, findings = check_complete(load_table(path), enums,
                                      base_manifests(), base_perf_names())
        check(not ok, "complete: nine sentinel marks in one config must fail")
        check(any(f.startswith("SENTINELS ctest-Debug: 9")
                  for f in findings),
              "complete: the over-marked config must be named, findings=%r"
              % findings)


def test_orphans_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, base_table_text())
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_names(), base_manifests())
        check(ok, "orphans: base stub table must pass, findings=%r"
              % findings)


def test_orphan_renamed_ctest_entry_fails():
    # Guard two: a renamed ctest entry leaves its reference rows pointing
    # at a name no tree enumerates any more.
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text().replace("alpha_probe_tests",
                                         "alpha_probe_tests_renamed")
        path = write_table(tmp, text)
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_names(), base_manifests())
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
        path = write_table(tmp, text)
        union = set().union(*base_enums().values())
        ok, findings = check_orphans(load_table(path), union,
                                     base_perf_names(), base_manifests())
        check(not ok, "orphans: a renamed manifest row must fail")
        check(any("manifest_probe_a_gone" in f for f in findings),
              "orphans: the renamed manifest entry must be named, findings=%r"
              % findings)


def test_constants_ok_on_base_table():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_table(tmp, base_table_text())
        ok, findings = check_constants(load_table(path))
        check(ok, "constants: the base stub table must pass, findings=%r"
              % findings)


def test_constants_sentinel_reference_above_min_cap_fails():
    # Guard seven: a sentinel whose reference exceeds MIN_CAP_S would be
    # killed by its own budget floor during calibration.
    with tempfile.TemporaryDirectory() as tmp:
        text = base_table_text() + ("ctest-Debug slow_sentinel_tests "
                                    "500.00 1 ctest-junit-serial yes "
                                    "over-long sentinel stub\n")
        path = write_table(tmp, text)
        ok, findings = check_constants(load_table(path))
        check(not ok, "constants: an over-long sentinel reference must fail")
        check(any("MIN_CAP_S" in f for f in findings),
              "constants: the MIN_CAP_S clause must be reported, "
              "findings=%r" % findings)


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
        table = write_table(tmp, base_table_text())
        readings = {"alpha_probe_tests": (10.0, "ctest-junit-serial"),
                    "beta_probe_tests": (20.0, "ctest-junit-serial"),
                    "gamma_probe_tests": (30.0, "ctest-junit-serial"),
                    "delta_probe_tests": (40.0, "ctest-junit-serial"),
                    "epsilon_probe_tests": (1000.0, "ctest-junit-serial")}
        orig = gate_time_check.sample_ctest
        gate_time_check.sample_ctest = _stub_ctest_sampler(readings)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                              str(Path(tmp) / "tree"), "--table", str(table)])
        finally:
            gate_time_check.sample_ctest = orig
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
            table = write_table(tmp, base_table_text())
            readings = {name: (secs, "ctest-junit-serial")
                        for name in CTEST_NAMES[:5]}
            orig = gate_time_check.sample_ctest
            gate_time_check.sample_ctest = _stub_ctest_sampler(readings)
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                                  str(Path(tmp) / "tree"),
                                  "--table", str(table)])
            finally:
                gate_time_check.sample_ctest = orig
            out = buf.getvalue()
            check(rc == 1 and "GATE gate_time_check: FAIL" in out,
                  "calibrate %s band: must exit red, got rc=%r out=%r"
                  % (label, rc, out))
            check("BAND ctest-Debug" in out,
                  "calibrate %s band: the config to re-record must be "
                  "named, got out=%r" % (label, out))
            check("CANDIDATE ctest-Debug alpha_probe_tests %.2f 1 "
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
        table = write_table(tmp, drop_line(base_table_text(),
                                           "ctest-Debug epsilon_probe_tests "))
        calls = []
        orig = gate_time_check.sample_ctest
        gate_time_check.sample_ctest = _stub_ctest_sampler({}, calls)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = run_cli(["calibrate", "ctest-Debug", "--tree",
                              str(Path(tmp) / "tree"), "--table", str(table)])
        finally:
            gate_time_check.sample_ctest = orig
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
        table = write_table(tmp, base_table_text())
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = run_cli(["calibrate", "e2e-Debug", "--tree",
                          str(Path(tmp) / "tree"), "--table", str(table)])
        out = buf.getvalue()
        check(rc == 1 and "--tools" in out,
              "calibrate: an e2e config with --tree must name --tools as "
              "the wanted flag, got rc=%r out=%r" % (rc, out))


def run():
    test_complete_ok_on_base_table()
    test_complete_missing_row_fails()
    test_complete_extra_row_fails()
    test_complete_sentinels_cleared_fails()
    test_complete_too_many_sentinels_fails()
    test_orphans_ok_on_base_table()
    test_orphan_renamed_ctest_entry_fails()
    test_orphan_renamed_e2e_entry_fails()
    test_constants_ok_on_base_table()
    test_constants_sentinel_reference_above_min_cap_fails()
    test_calibrate_median_and_scale_line()
    test_calibrate_out_of_band_red_with_candidates()
    test_calibrate_fewer_than_five_sentinels_red_without_sampling()
    test_calibrate_flag_config_mismatch_red()
