# gate_time_perf_test.py -- perf-class cases for the schedule axis
# (docs/dev/gate_timing_design.md D-6, guards eight and eleven): print-
# perf-names computes the perf entry set as the --show-only difference
# of two scratch configures (flag OFF then ON, seed cache flags
# forwarded), check-schedule --perf-names pins guard eight (R-61):
# every differential name owns a priced ctest-Release row (a row
# without the sentinel mark) and no perf name may appear in any T0/T1
# run enumeration. The check-carrier cases (guard eleven: every slow
# member's row and every ASan/Release row paired with the leg that ran
# it under the row's own config key -- the ctest config from the tree
# cache plus -C, the e2e config from the leg's explicit --config
# value, the sentinel-marked ctest-Release rows excepted because guard
# four's calibration runs those, D-19) live in gate_time_carrier_test,
# which reuses this module's stub cmake/ctest doors (the import reads
# one-way: perf test first, then carrier test).
# Stub cmake/ctest .bat files
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
        if os.name == "nt":
            bat.write_text('@echo off\r\n"%s" -I "%s" %%*\r\n'
                           % (sys.executable, tools / script),
                           encoding="ascii")
        else:
            # POSIX cannot spawn a .bat body: the same stub ships as an
            # executable shebang script forwarding "$@" to the stub.
            bat.write_text('#!/bin/sh\nexec "%s" -I "%s" "$@"\n'
                           % (sys.executable, tools / script),
                           encoding="utf-8")
            bat.chmod(0o755)
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


def run():
    test_print_perf_names_is_the_registration_differential()
    test_print_perf_names_refusals_name_their_file_or_side()
    test_check_schedule_perf_names_priced_rows_and_run_set()
    test_check_schedule_perf_names_reads_the_raw_redirect()
