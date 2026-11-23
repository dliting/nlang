# gate_time_carrier_test.py -- carrier cases for the check-carrier
# subcommand (guard eleven, docs/dev/gate_timing_design.md D-17):
# the round's leg records (leg_commands.tsv) must pair every slow
# member's row -- under every config key it is priced in -- and every
# ASan/Release row with the leg that ran it under the row's own config
# key (the ctest config from the tree cache plus -C, the e2e config
# from the leg's explicit --config value), the sentinel-marked ctest-
# Release rows excepted because guard four's calibration runs those
# (D-19). The fixture builds the three stub trees, both e2e manifests
# and the perf differential behind gate_time_perf_test's stub cmake/
# ctest doors (the import reads one-way: perf test first, then carrier
# test). Entry point: gate_time_check_test.py (its main() calls run()).
# Stdlib only; run with -I.

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_perf_test import PERF_TABLE, _doors, _run, _stub_pair
from gate_time_testutil import check, write_table


# The carrier fixture's table: the perf rows of PERF_TABLE, the two coarse
# scope rows whose carrier is an e2e leg's manifest file (an ASan or
# Release row pairs by its config key alone, D-17), the slow
# e2e_runner_timing_gate's e2e-Debug row removed (guard eleven pairs a
# slow member's row under EVERY config, and this stub round records no
# e2e-Debug leg), and a ghost entry priced under exactly the
# ctest-Debug and ctest-IDE keys -- the mispairing pin moves it out of
# one tree and requires the other config's row to stay green, which
# needs both rows to exist.
CARRIER_TABLE = PERF_TABLE.replace(
    "e2e-Debug e2e_runner_timing_gate 50.00 1 e2e-runner-serial - "
    "timing probes carried every round elsewhere\n", "") + (
    "e2e-ASan manifest.txt 100.00 1 e2e-runner-serial - full "
    "sweep carried in T2\n"
    "e2e-Release examples_manifest.txt 10.00 1 e2e-runner-"
    "serial - examples carried by the release e2e leg\n"
    "ctest-Debug ghost_debug_entry 500.00 1 ctest-junit-serial "
    "- debug unit leg carries it every round\n"
    "ctest-IDE ghost_debug_entry 500.00 1 ctest-junit-serial "
    "- IDE ctest leg carries it every round\n")


def _perf_file(tmp, names):
    path = Path(tmp) / "perf_names.txt"
    path.write_text(names + "\n", encoding="utf-8")
    return str(path)



def _carrier_lines(p):
    def ctest_leg(tree):
        return ("env NLANG_GATE_MACHINE_SCALE=1.1 PYTHONIOENCODING=utf-8"
                " bash -c \"cd '%s' && ctest -C Debug --output-junit "
                "'%s/j.xml' --timeout 1500\"" % (tree, tree))
    e2e = "%s %s ncc.exe nvm.exe ndb.exe" % (sys.executable,
                                             p["e2e_script"])
    return [
        ("T1-debug-unit", ctest_leg(p["dbg"])),
        ("T1-ide-ctest", ctest_leg(p["ide"])),
        ("T1-asan-unit", ctest_leg(p["asan"])),
        ("T2-asan-e2e-full", "env PYTHONIOENCODING=utf-8 NLANG_E2E_"
         "MANIFEST=%s %s --timeout=180 --config=ASan"
         % (p["e2e_manifest"], e2e)),
        ("T2-release-e2e", "env PYTHONIOENCODING=utf-8 %s "
         "--timeout=120 --config=Release" % e2e),
        ("T2-perf", "env NLANG_GATE_MACHINE_SCALE=1.1 PYTHONIOENCODING"
         "=utf-8 bash -c \"cmake -S 'R' -B '%s' -DNLANG_PERF_TESTS=ON "
         "&& ctest -C Release -R '^(regex_redos_perf_compile|"
         "regex_redos_perf_run)$' --output-junit '%s/p.xml' --timeout "
         "1500\"" % (p["dbg"], p["dbg"])),
        ("T2-perf-timeouts", "env PYTHONIOENCODING=utf-8 %s -I %s "
         "check-timeouts --tree '%s' --table '%s'"
         % (sys.executable,
            str(Path(p["table"]).parent / "gate_time_check.py"),
            p["dbg"], p["table"])),
    ]


def _write_records(path, lines):
    Path(path).write_text("".join("%s\t%s\n" % (n, c)
                                  for n, c in lines), encoding="utf-8")


def _carrier(tmp):
    """A T2 round's leg records with the trees and manifests they read:
    three stub trees (plain, IDE-shaped and sanitizer-shaped
    CMakeCache.txt, per-tree entries.txt), an e2e script directory with
    both manifests, and the perf differential file. The dict also
    carries `lines`, the records' (name, command) pairs, so a variant
    drops or rewrites one leg and reruns."""
    base = Path(tmp)
    p = {}
    p["cmake"], p["ctest"] = _stub_pair(tmp)
    p["table"] = str(write_table(tmp, CARRIER_TABLE))
    p["perf"] = _perf_file(tmp, "regex_redos_perf_compile|"
                           "regex_redos_perf_run")
    trees = {}
    for key, cache in (
            ("dbg", "CMAKE_GENERATOR:INTERNAL=VS\nNLANG_BUILD_IDE:"
                    "BOOL=OFF\n"),
            ("ide", "CMAKE_GENERATOR:INTERNAL=VS\nNLANG_BUILD_IDE:"
                    "BOOL=ON\n"),
            ("asan", "CMAKE_GENERATOR:INTERNAL=VS\nCMAKE_CXX_FLAGS:"
                     "STRING=-fsanitize=address\n")):
        tree = base / ("tree-" + key)
        tree.mkdir()
        (tree / "CMakeCache.txt").write_text(cache, encoding="utf-8")
        p[key] = tree.as_posix()
        trees[key] = tree
    # The stub trees enumerate what every paired row needs: the slow
    # members' rows exist under all three ctest configs (mainwindow_tests
    # included, D-17's membership is global), and the 800 s sanitizer-
    # only entry under its own config.
    for key, names in (
            ("dbg", ("plain_unit_entry", "e2e_runner_timing_gate",
                     "mnist_mlp_compile", "mainwindow_tests",
                     "ghost_debug_entry")),
            ("ide", ("e2e_runner_timing_gate", "mnist_mlp_compile",
                     "mainwindow_tests", "ghost_debug_entry")),
            ("asan", ("mainwindow_tests", "asan_heavy_entry",
                      "plain_unit_entry"))):
        (trees[key] / "entries.txt").write_text(
            "".join(n + "\n" for n in names), encoding="utf-8")
    e2e = base / "e2e"
    e2e.mkdir()
    (e2e / "run_e2e_tests.py").write_text("# stub\n", encoding="utf-8")
    (e2e / "manifest.txt").write_text("#stub manifest\nsmoke_one 0\n"
                                      "e2e_stub_demo 3\n",
                                      encoding="utf-8")
    (e2e / "examples_manifest.txt").write_text("#stub examples\n"
                                               "examples_demo 1\n",
                                               encoding="utf-8")
    # Slash-wise like the bash-recorded commands: unquoted env tokens
    # (NLANG_E2E_MANIFEST=...) would lose native backslashes to shlex.
    p["e2e_script"] = (e2e / "run_e2e_tests.py").as_posix()
    p["e2e_manifest"] = (e2e / "manifest.txt").as_posix()
    p["rec"] = str(base / "leg_commands.tsv")
    p["lines"] = _carrier_lines(p)
    _write_records(Path(p["rec"]), p["lines"])
    return p


def _carrier_run(p, lines=None, perf=True):
    if lines is not None:
        _write_records(Path(p["rec"]), lines)
    argv = ["check-carrier", "--table", str(p["table"]),
            "--leg-commands", p["rec"]]
    if perf:
        argv += ["--perf-names", p["perf"]]
    with _doors(p["cmake"], p["ctest"]):
        return _run(argv)


def test_check_carrier_pairs_every_guarded_row_with_its_config():
    # Guard eleven (D-17): every row of a slow member and every
    # ASan/Release row needs a leg whose judging config and run set
    # pair it -- the ctest config from the tree
    # cache plus -C (the same shape predicate tests/gate_time_keys.
    # cmake applies), the e2e config from the leg's explicit --config
    # value, the run set from the tree enumeration or (the perf leg,
    # whose ON-cache window has closed by the time this checks) from
    # the differential file, and an e2e leg's set as its manifest plus
    # the manifest FILES' names (scope rows pair against basenames).
    # Dropping the perf leg names both perf rows; dropping the full-
    # sweep e2e leg names the scope row whose carrier was its manifest
    # basename -- the release leg runs the same manifest file but
    # under another config key and must not close it.
    with tempfile.TemporaryDirectory() as tmp:
        p = _carrier(tmp)
        rc, out = _carrier_run(p)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "guard eleven: the T2 round's records must pair every "
              "slow-member and ASan/Release row, got rc=%r out=%r"
              % (rc, out))
        rc, out = _carrier_run(p, [kv for kv in p["lines"]
                                   if kv[0] != "T2-perf"])
        check(rc == 1 and "CARRIER" in out and "ctest-Release" in out
              and "regex_redos_perf_compile" in out
              and "regex_redos_perf_run" in out,
              "guard eleven: a perf row without its T2 leg must name "
              "both entries, got rc=%r out=%r" % (rc, out))
        rc, out = _carrier_run(p, [kv for kv in p["lines"]
                                   if kv[0] != "T2-asan-e2e-full"])
        check(rc == 1 and "CARRIER" in out and "e2e-ASan" in out
              and "manifest.txt" in out,
              "guard eleven: the scope row's only carrier is the leg "
              "running that manifest under its config, got rc=%r out=%r"
              % (rc, out))


def test_check_carrier_refuses_mispaired_configs():
    # The pairing is strict on both sides. Same name, other config:
    # ghost_debug_entry moves out of the debug tree while the IDE tree
    # keeps it -- the ctest-Debug row goes red, the ctest-IDE row
    # stays green (a name in SOME leg proves nothing). Shape over
    # word: -C Release cannot promote the sanitizer tree out of
    # ctest-ASan (the cache's fsanitize wins, F-1). No word at all: an
    # e2e leg without --config= has no judging config, named by its
    # leg name and the missing flag.
    with tempfile.TemporaryDirectory() as tmp:
        p = _carrier(tmp)
        (Path(p["dbg"]) / "entries.txt").write_text(
            "plain_unit_entry\ne2e_runner_timing_gate\n",
            encoding="utf-8")
        rc, out = _carrier_run(p)
        check(rc == 1 and "ctest-Debug ghost_debug_entry" in out
              and "ctest-IDE ghost_debug_entry" not in out,
              "guard eleven: the same name under another config must "
              "not close the row, got rc=%r out=%r" % (rc, out))
        lines = [(n, c.replace("ctest -C Debug", "ctest -C Release")
                  if n == "T1-asan-unit" else c)
                 for n, c in p["lines"]]
        rc, out = _carrier_run(p, lines)
        check(rc == 1 and "CARRIER" in out and "ctest-ASan" in out
              and "Release" in out,
              "guard eleven: a sanitizer tree stays ctest-ASan whatever "
              "-C says, got rc=%r out=%r" % (rc, out))
        lines = [(n, c.replace(" --config=ASan", ""))
                 if n == "T2-asan-e2e-full" else (n, c)
                 for n, c in p["lines"]]
        rc, out = _carrier_run(p, lines)
        check(rc == 1 and "T2-asan-e2e-full" in out and "--config" in out,
              "guard eleven: an e2e leg without --config= must name its "
              "leg and the missing flag, got rc=%r out=%r" % (rc, out))


def test_check_carrier_named_refusals_on_missing_inputs():
    # Inputs the checker owes dispositions for, not tracebacks: an
    # ON-flagged leg without --perf-names cannot know its run set (the
    # post-restore enumeration holds no perf entries -- it would read
    # an innocent empty set and green-light the missing carrier), and
    # a tree without a CMakeCache.txt has no config shape to pair
    # against. Both ordinary findings naming what is missing.
    with tempfile.TemporaryDirectory() as tmp:
        p = _carrier(tmp)
        rc, out = _carrier_run(p, perf=False)
        check(rc == 1 and "CARRIER" in out and "T2-perf" in out
              and "perf-names" in out,
              "guard eleven: an ON leg needs the differential file, got "
              "rc=%r out=%r" % (rc, out))
        (Path(p["asan"]) / "CMakeCache.txt").unlink()
        rc, out = _carrier_run(p)
        check(rc == 1 and "CARRIER" in out and "CMakeCache.txt" in out,
              "guard eleven: a tree without a cache is a named finding, "
              "got rc=%r out=%r" % (rc, out))



def test_check_carrier_tolerates_every_perf_roster_spelling():
    # R-80: check-carrier reads --perf-names through the same shared
    # reader as the other three consumers. Its perf-leg run set is the
    # differential intersected with the leg's -R regex, so a roster in
    # any of the print-perf-names spellings -- the '|'-joined value line
    # (the production form the fixture already carries), one name per
    # line, or the raw stdout redirect with the trailing 'GATE ' verdict
    # line -- selects the same two perf entries and the round stays
    # green. This case is the pin for the swap from the carrier's own
    # inline split: a stray extra name reaches no -R alternative, so the
    # tolerance it shows here is exactly the tolerance it must keep.
    with tempfile.TemporaryDirectory() as tmp:
        p = _carrier(tmp)
        for spelling, text in (
                ("joined value line", "regex_redos_perf_compile|"
                 "regex_redos_perf_run\n"),
                ("per line", "regex_redos_perf_compile\n"
                 "regex_redos_perf_run\n"),
                ("raw redirect", "regex_redos_perf_compile|"
                 "regex_redos_perf_run\n"
                 "GATE gate_time_check: PASS\n")):
            Path(p["perf"]).write_text(text, encoding="utf-8")
            rc, out = _carrier_run(p)
            check(rc == 0 and "GATE gate_time_check: PASS" in out,
                  "guard eleven: the perf roster in the %s spelling "
                  "must still pair every perf row, got rc=%r out=%r"
                  % (spelling, rc, out))


def test_check_carrier_calibration_sentinels_need_no_leg():
    # D-19: a sentinel-marked ctest-Release row is carried by guard
    # four's calibration, not by a leg of leg_commands.tsv -- the perf
    # family's legs sample the ctest-Release scale from exactly these
    # rows in the load form, and a failed calibration stubs that leg
    # red (the uncalibrated `false` face), so the batch still fails
    # closed. Guard one already prices only the perf rows and the
    # sentinels under ctest-Release; guard eleven's only input is
    # leg_commands.tsv, where a calibration never appears, so it must
    # not demand a leg carrier for them. A non-sentinel ctest-Release
    # row outside the perf differential stays flagged.
    with tempfile.TemporaryDirectory() as tmp:
        p = _carrier(tmp)
        table = Path(p["table"])
        text = table.read_text(encoding="utf-8")
        table.write_text(
            text + "ctest-Release release_calib_sentinel 0.02 1 "
            "ctest-junit-serial yes calibration sentinel face\n"
            "ctest-Release release_plain_entry 0.03 1 ctest-junit-"
            "serial - priced but never run\n", encoding="utf-8")
        rc, out = _carrier_run(p)
        check(rc == 1 and "CARRIER ctest-Release release_plain_entry"
              in out and "release_calib_sentinel" not in out,
              "D-19: the sentinel-marked ctest-Release row must not "
              "need a leg carrier while an unmarked one still does, "
              "got rc=%r out=%r" % (rc, out))



def run():
    test_check_carrier_pairs_every_guarded_row_with_its_config()
    test_check_carrier_refuses_mispaired_configs()
    test_check_carrier_named_refusals_on_missing_inputs()
    test_check_carrier_tolerates_every_perf_roster_spelling()
    test_check_carrier_calibration_sentinels_need_no_leg()
