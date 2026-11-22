# gate_time_perf_test.py -- perf-class cases for the schedule axis
# (docs/dev/gate_timing_design.md D-6, guards eight and eleven): print-
# perf-names computes the perf entry set as the --show-only difference
# of two scratch configures (flag OFF then ON, seed cache flags
# forwarded), check-schedule --perf-names pins guard eight (R-61):
# every differential name owns a priced ctest-Release row (a row
# without the sentinel mark) and no perf name may appear in any T0/T1
# run enumeration; check-carrier pairs every slow member's row and
# every ASan/Release row with the leg that runs it under the row's own
# config
# key (the ctest config from the tree cache plus -C, the e2e config
# from the leg's explicit --config value). Stub cmake/ctest .bat files
# behind the NLANG_GATE_CMAKE / NLANG_GATE_CTEST doors keep the corpus
# hermetic: no real configure, no tree. Entry point:
# gate_time_check_test.py (its main() calls run()). Stdlib only; run
# with -I.

import io
import json
import os
import sys
import tempfile
from contextlib import contextmanager, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_testutil import check, write_table
from gate_time_schedule_test import (SCHEDULE_TABLE, _fixture, _schedule,
                                     _show_only_doc)

# The perf class as the committed table prices it (the ctest-Release
# rows without the sentinel mark -- the priced set guard eight compares
# the registration difference against, R-61), appended to the schedule
# stub table so the cases see both views of the set.
PERF_TABLE = SCHEDULE_TABLE + (
    "ctest-Release regex_redos_perf_compile 0.04 1 ctest-junit-"
    "serial - perf class registered only under the ON flag\n"
    "ctest-Release regex_redos_perf_run 7.40 1 ctest-junit-"
    "serial - perf class registered only under the ON flag\n")

# The carrier fixture's table: the perf rows above, the two coarse
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

# A seed cache shaped like a real VS configure: generator keys (one
# empty toolset), one plain FILEPATH entry, one underscore entry, one
# INTERNAL entry and the perf flag itself -- only the generator pair
# and the plain entry may reach the scratch configure.
SEED_CACHE = """\
# CMakeCache.txt stub
CMAKE_GENERATOR:INTERNAL=Stub Gen
CMAKE_GENERATOR_PLATFORM:INTERNAL=x64
CMAKE_GENERATOR_TOOLSET:INTERNAL=
SEED_TOOL:FILEPATH=C:/stub/tool
_HIDDEN:INTERNAL=keep
NLANG_PERF_TESTS:BOOL=OFF
CMAKE_HOME_DIR:INTERNAL=C:/stub
"""

STUB_CMAKE = '''\
import json, os, sys
a = sys.argv[1:]
b = a[a.index("-B") + 1]
os.makedirs(b, exist_ok=True)
with open(os.path.join(b, "argv.txt"), "w") as fh:
    json.dump(a, fh)
flag = "none"
for t in a:
    if t.startswith("-DNLANG_PERF_TESTS="):
        flag = t.split("=", 1)[1]
with open(os.path.join(b, "perf.flag"), "w") as fh:
    fh.write(flag)
if os.environ.get("STUB_CMAKE_FAIL") == "1":
    sys.stderr.write("stub configure failed\\n")
    sys.exit(1)
'''

STUB_CTEST = '''\
import json, os, sys
a = sys.argv[1:]
tree = a[a.index("--test-dir") + 1]
entries = os.path.join(tree, "entries.txt")
if os.path.isfile(entries):
    with open(entries) as fh:
        names = [ln.split()[0] for ln in fh if ln.strip()]
else:
    names = ["plain_unit_entry"]
flag = os.path.join(tree, "perf.flag")
if os.path.isfile(flag):
    with open(flag) as fh:
        if fh.read().strip() == "ON":
            names += ["regex_redos_perf_compile", "regex_redos_perf_run",
                      "perf_ghost_build_only"]
print(json.dumps({"tests": [{"name": n, "properties": []}
                            for n in names]}))
'''


def _stub_pair(tmp):
    """Stub cmake/ctest behind the NLANG_GATE_CMAKE / NLANG_GATE_CTEST
    doors -- .bat form because CreateProcess spawns no bash script, the
    same shape the timeouts corpus pins. cmake records its argv and the
    -DNLANG_PERF_TESTS flag it was handed into the build directory;
    STUB_CMAKE_FAIL=1 fails the configure after recording; ctest
    enumerates <tree>/entries.txt plus, while the flag file reads ON,
    three perf names (one no table row prices, to prove the printed
    differential was never intersected with the table)."""
    tools = Path(tmp) / "stub-tools"
    tools.mkdir()
    for name, text in (("stub_cmake.py", STUB_CMAKE),
                       ("stub_ctest.py", STUB_CTEST)):
        (tools / name).write_text(text, encoding="utf-8")
    pair = {}
    for label, script in (("cmake", "stub_cmake.py"),
                          ("ctest", "stub_ctest.py")):
        bat = tools / (label + ".bat")
        bat.write_text('@echo off\r\n"%s" -I "%s" %%*\r\n'
                       % (sys.executable, tools / script),
                       encoding="ascii")
        pair[label] = str(bat)
    return pair["cmake"], pair["ctest"]


@contextmanager
def _doors(cmake, ctest, fail=False):
    saved = dict(os.environ)
    os.environ["NLANG_GATE_CMAKE"] = cmake
    os.environ["NLANG_GATE_CTEST"] = ctest
    if fail:
        os.environ["STUB_CMAKE_FAIL"] = "1"
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)


def _run(argv):
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            rc = run_cli(argv)
    except SystemExit as exc:
        rc = exc.code
    return rc, buf.getvalue()


def _flag_value(argv, flag):
    return argv[argv.index(flag) + 1] if flag in argv else None


def _perf_file(tmp, names):
    path = Path(tmp) / "perf_names.txt"
    path.write_text(names + "\n", encoding="utf-8")
    return str(path)


def test_print_perf_names_is_the_registration_differential():
    # D-6: the perf name set comes from the registration itself, never
    # the table -- one scratch tree configured twice (flag OFF then ON)
    # with the seed tree's cache flags forwarded so the scratch
    # configures like the real sweep tree, and the --show-only name
    # difference IS the set, printed as one value line (print-slow's
    # shape). The stub's extra perf_ghost_build_only (no table row
    # prices it) pins that nothing was intersected with the table, and
    # the seed's own NLANG_PERF_TESTS cache entry may contribute no
    # second flag of its side (exactly one flag per configure, its own
    # value).
    with tempfile.TemporaryDirectory() as tmp:
        cmake, ctest = _stub_pair(tmp)
        seed = Path(tmp) / "seed"
        seed.mkdir()
        (seed / "CMakeCache.txt").write_text(SEED_CACHE, encoding="utf-8")
        scratch = Path(tmp) / "scratch"
        with _doors(cmake, ctest):
            rc, out = _run(["print-perf-names", "--seed-tree", str(seed),
                            "--scratch", str(scratch)])
        values = [ln for ln in out.splitlines()
                  if ln and not ln.startswith("GATE ")]
        check(rc == 0 and values == ["perf_ghost_build_only|"
                                     "regex_redos_perf_compile|"
                                     "regex_redos_perf_run"]
              and "GATE gate_time_check: PASS" in out,
              "print-perf-names: want the ON-OFF difference as one "
              "value line, got rc=%r out=%r" % (rc, out))
        flags = {}
        for sub in sorted(Path(scratch).iterdir()):
            flags[sub.name] = (sub / "perf.flag").read_text(
                encoding="utf-8")
        check(set(flags.values()) == {"ON", "OFF"},
              "print-perf-names: the scratch must hold one ON and one "
              "OFF configure, got %r" % (sorted(flags.items()),))
        for name, want in sorted(flags.items()):
            argv = json.loads((Path(scratch) / name / "argv.txt")
                              .read_text(encoding="utf-8"))
            perf = [t for t in argv
                    if t.startswith("-DNLANG_PERF_TESTS")]
            forwarded = [t[2:].split("=")[0] for t in argv
                         if t.startswith("-D")]
            check(perf == ["-DNLANG_PERF_TESTS=" + want]
                  and _flag_value(argv, "-G") == "Stub Gen"
                  and _flag_value(argv, "-A") == "x64"
                  and "-T" not in argv
                  and "-DSEED_TOOL=C:/stub/tool" in argv
                  and sorted(set(forwarded) - {"NLANG_PERF_TESTS"})
                      == ["SEED_TOOL"],
                  "print-perf-names: the seed cache must forward the "
                  "generator pair and non-INTERNAL entries only, got %r"
                  % (argv,))


def test_print_perf_names_refusals_name_their_file_or_side():
    # Two dispositions, neither a traceback: a seed tree without a
    # CMakeCache.txt cannot say which generator and flags the scratch
    # repeats -- an ordinary verdict naming the file and --seed-tree
    # (main()'s OSError funnel would quote only the OS message, the
    # cleanup-three lesson). A failing configure names the side that
    # died (the ON configure runs first); FAIL (1 findings), not
    # FAIL (error).
    with tempfile.TemporaryDirectory() as tmp:
        cmake, ctest = _stub_pair(tmp)
        seed = Path(tmp) / "seed"
        seed.mkdir()
        scratch = Path(tmp) / "scratch"
        with _doors(cmake, ctest):
            rc, out = _run(["print-perf-names", "--seed-tree", str(seed),
                            "--scratch", str(scratch)])
        check(rc == 1 and "CMakeCache.txt" in out and "seed" in out
              and "FAIL (1 findings)" in out and "FAIL (error)" not in out,
              "print-perf-names: an unseeded tree must be refused by "
              "name, got rc=%r out=%r" % (rc, out))
        (seed / "CMakeCache.txt").write_text(SEED_CACHE, encoding="utf-8")
        with _doors(cmake, ctest, fail=True):
            rc, out = _run(["print-perf-names", "--seed-tree", str(seed),
                            "--scratch", str(scratch)])
        check(rc == 1 and "FAIL (1 findings)" in out and "configure" in out
              and "ON" in out and "FAIL (error)" not in out,
              "print-perf-names: a failing configure must name its side, "
              "got rc=%r out=%r" % (rc, out))


def test_check_schedule_perf_names_priced_rows_and_run_set():
    # Guard eight (R-61): the ON/OFF registration difference is the one
    # truth about the perf class; the table only prices it. Two
    # verdicts remain: a difference name with no priced ctest-Release
    # row (no row, or one marked sentinel=yes) would run in the regular
    # legs unpriced, and a perf name inside a T0/T1 run enumeration
    # means that round's -E lost the perf class (the --show-only doc
    # stands in for the leg's run set). The old third direction -- a
    # priced row missing from the differential, an entry running in no
    # leg -- is guard one's EXTRA ctest-Release verdict under R-43
    # (Task 10 Step 5), so the half-differential stays silent HERE; the
    # case pins that silence.
    with tempfile.TemporaryDirectory() as tmp:
        _table, families, root = _fixture(tmp)
        perf_table = write_table(tmp, PERF_TABLE)
        doc = Path(tmp) / "doc.json"
        pair = ["regex_redos_perf_compile", "regex_redos_perf_run"]

        def run(names, doc_names=None):
            pf = Path(tmp) / "perf.txt"
            pf.write_text("|".join(names) + "\n", encoding="utf-8")
            extra = ["--config", "ctest-Debug", "--perf-names", str(pf)]
            if doc_names is not None:
                doc.write_text(_show_only_doc(doc_names),
                               encoding="utf-8")
                extra += ["--show-only", str(doc)]
            return _schedule(perf_table, families, root, extra)

        rc, out = run(pair)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "guard eight: a differential whose every name has a "
              "priced row must pass, got rc=%r out=%r" % (rc, out))
        rc, out = run(pair[:1])
        check(rc == 0 and "PERF" not in out,
              "guard eight: a priced row missing from the differential "
              "is guard one's EXTRA direction (R-43), no finding here, "
              "got rc=%r out=%r" % (rc, out))
        rc, out = run(pair + ["perf_ghost_build_only"])
        check(rc == 1 and "PERF" in out and "perf_ghost_build_only" in out,
              "guard eight: a differential name with no priced row must "
              "be named, got rc=%r out=%r" % (rc, out))
        rc, out = run(pair, ["plain_unit_entry", "regex_redos_perf_run"])
        check(rc == 1 and "PERF" in out and "regex_redos_perf_run" in out,
              "guard eight: a perf name inside a T0/T1 run enumeration "
              "must be named, got rc=%r out=%r" % (rc, out))
        rc, out = run(pair, ["plain_unit_entry"])
        check(rc == 0, "guard eight: perf names absent from the run set "
              "must pass, got rc=%r out=%r" % (rc, out))


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


def test_check_schedule_perf_names_reads_the_raw_redirect():
    # R-80 (spec D-17's format contract): check-schedule's --perf-names
    # reader is the shared roster reader, so the raw stdout redirect of
    # print-perf-names -- the value line plus the trailing 'GATE '
    # verdict line, the shape a shell `> file` capture leaves behind --
    # reads as the differential itself. Before the shared reader the
    # verdict line became a third difference name with no priced
    # ctest-Release row, and guard eight went red quoting it.
    with tempfile.TemporaryDirectory() as tmp:
        _table, families, root = _fixture(tmp)
        perf_table = write_table(tmp, PERF_TABLE)
        pf = Path(tmp) / "perf_raw.txt"
        pf.write_text("regex_redos_perf_compile|regex_redos_perf_run\n"
                      "GATE gate_time_check: PASS\n", encoding="utf-8")
        rc, out = _schedule(perf_table, families, root,
                            ["--config", "ctest-Debug",
                             "--perf-names", str(pf)])
        check(rc == 0 and "GATE gate_time_check: PASS" in out
              and "PERF" not in out,
              "guard eight: the raw redirect of print-perf-names must "
              "read as the differential, got rc=%r out=%r" % (rc, out))


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


def run():
    test_print_perf_names_is_the_registration_differential()
    test_print_perf_names_refusals_name_their_file_or_side()
    test_check_schedule_perf_names_priced_rows_and_run_set()
    test_check_carrier_pairs_every_guarded_row_with_its_config()
    test_check_carrier_refuses_mispaired_configs()
    test_check_carrier_named_refusals_on_missing_inputs()
    test_check_schedule_perf_names_reads_the_raw_redirect()
    test_check_carrier_tolerates_every_perf_roster_spelling()
