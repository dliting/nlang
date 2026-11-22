# gate_time_schedule_test.py -- schedule-axis cases for
# gate_time_schedule.py (guard six and guard nine, docs/dev/
# gate_timing_design.md D-7): print-slow builds the per-config -E regex
# from the category column and subtracts what the leg's own -R mapping
# will run again (ctest -E beats -R, F-12); check-schedule pins the
# backfill equivalence ctest itself computes (a saved --show-only doc
# stands in for the leg), the 甲/乙 recovery path every category!=常规
# row must own, the >SLOW_MIN_REF_S row that stays 常规 without a
# reason, the phantom src glob (guard nine) and the e2e set formula
# "explicit-常规 rows union mapping hits, manifest order, deduped".
# Stub tables, stub family files and saved ctest --show-only documents
# keep the corpus hermetic: no ctest run, no tree. Entry point:
# gate_time_check_test.py (its main() calls run()). Stdlib only; run
# with -I.

import io
import json
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_table import load_table
from gate_time_testutil import check, write_table
from gate_time_schedule import (derive_e2e_subset, slow_names,
                                slow_regex)

# Stub reference table for the schedule cases. The category column is
# the whole input: two 慢功能/演示验收 rows under ctest-Debug (both with
# their reason filled, so the backfill case sees no claim finding), one
# 慢功能 row under ctest-ASan, an e2e-Debug universe whose 常规 branch
# is exactly the stdlib row and whose 演示验收 row stays out unless the
# mapping hits it, plus the default rows that must NEVER feed the -E
# regex or the e2e subset branch.
SCHEDULE_TABLE = """\
# stub schedule table
ctest-Debug default 10.00 1 常规 ctest-junit-serial - stub
ctest-Debug plain_unit_entry 5.00 1 常规 ctest-junit-serial - stub
ctest-Debug e2e_runner_timing_gate 409.77 1 慢功能 ctest-junit-serial - timing probes carried every round elsewhere
ctest-Debug mnist_mlp_compile 220.00 1 演示验收 ctest-junit-serial - demo class carried by T2
ctest-IDE default 10.00 1 常规 ctest-junit-serial - stub
ctest-IDE e2e_runner_timing_gate 409.80 1 慢功能 ctest-junit-serial - timing probes carried every round elsewhere
ctest-IDE mnist_mlp_compile 240.00 1 演示验收 ctest-junit-serial - demo class carried by T2
ctest-ASan default 10.00 1 常规 ctest-junit-serial - stub
ctest-ASan mainwindow_tests 372.19 1 慢功能 ctest-junit-serial - instrumented face carried by T2
e2e-Debug default 10.00 1 常规 e2e-runner-serial - stub
e2e-Debug stdlib_stub_size 30.00 1 常规 e2e-runner-serial - stub reason
e2e-Debug e2e_stub_demo 40.00 1 演示验收 e2e-runner-serial - demo class carried by T2
"""

# The tensor family as D-7 甲 specifies it: a src rule and a unit
# fragment that re.search-hits the 演示验收 entry name.
TENSOR_FAMILIES = """\
# stub families
family tensor src src/native/tensor/**
family tensor unit mnist_mlp_compile|mnist_mlp_run
"""

E2E_MANIFEST = """\
#stub manifest
smoke_one 0
stdlib_stub_size 5
e2e_stub_demo 3
"""


def _show_only_doc(names):
    """A saved ctest --show-only=json-v1 document: exactly the entries a
    leg with its -R/-E pair would run (the doc IS the equivalence ctest
    computes; the guard only reads the set)."""
    return json.dumps({"tests": [{"name": n, "properties": []}
                                 for n in names]})


def _cli(argv):
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            rc = run_cli(argv)
    except SystemExit as exc:
        # Unregistered subcommands exit through argparse before main's
        # error handler; record the code so the case reports it.
        rc = exc.code
    return rc, buf.getvalue()


def _fixture(tmp):
    """Stub table + family file + a repo root holding the tensor src
    file (so the 甲 src rule is live and guard nine passes for the
    tensor glob)."""
    table = write_table(tmp, SCHEDULE_TABLE)
    families = Path(tmp) / "families.txt"
    families.write_text(TENSOR_FAMILIES, encoding="utf-8")
    root = Path(tmp) / "repo"
    (root / "src" / "native" / "tensor").mkdir(parents=True)
    (root / "src" / "native" / "tensor" / "TensorCore.cpp").write_text(
        "// stub\n", encoding="utf-8")
    return table, families, root


def _schedule(table, families, root, extra):
    return _cli(["check-schedule", "--table", str(table),
                 "--families", str(families), "--repo-root", str(root)]
                + list(extra))


def test_slow_regex_per_config_and_keep_matched():
    # print-slow: the joined -E regex is the config's category!=常规 set
    # (sorted, single site); the default rows never join it.
    # --keep-matched subtracts whatever the leg's -R regex re.search-hits
    # (ctest partial-match semantics), so a mapped slow entry stays
    # runnable (F-12). "." matches every name, emptying the regex.
    with tempfile.TemporaryDirectory() as tmp:
        rows = load_table(write_table(tmp, SCHEDULE_TABLE))
        check(slow_names(rows, "ctest-Debug")
              == ["e2e_runner_timing_gate", "mnist_mlp_compile"],
              "slow_names: ctest-Debug must list both exception rows, "
              "got %r" % (slow_names(rows, "ctest-Debug"),))
        check(slow_names(rows, "ctest-ASan") == ["mainwindow_tests"],
              "slow_names: the ASan config sees only its own rows, got "
              "%r" % (slow_names(rows, "ctest-ASan"),))
        check(slow_regex(rows, "ctest-Debug")
              == "e2e_runner_timing_gate|mnist_mlp_compile",
              "slow_regex: want the sorted joined regex, got %r"
              % slow_regex(rows, "ctest-Debug"))
        check(slow_regex(rows, "ctest-Debug",
                         "mnist_mlp_compile")
              == "e2e_runner_timing_gate",
              "slow_regex: a keep-matched regex must subtract the hit "
              "entry (F-12 backfill), got %r"
              % slow_regex(rows, "ctest-Debug", "mnist_mlp_compile"))
        # ctest partial match: the mapping regex "plain" as a fragment
        # must hit nothing here, "e2e_runner" must hit its entry.
        check(slow_regex(rows, "ctest-Debug", "mnist_mlp")
              == "e2e_runner_timing_gate",
              "slow_regex: keep-matched is a partial match like ctest "
              "-R, got %r" % slow_regex(rows, "ctest-Debug", "mnist_mlp"))
        check(slow_regex(rows, "ctest-Debug", ".") == "",
              "slow_regex: a '.' mapping hits every entry, leaving no "
              "exclusion, got %r" % slow_regex(rows, "ctest-Debug", "."))
        rc, out = _cli(["print-slow", "ctest-Debug", "--table",
                        str(write_table(tmp, SCHEDULE_TABLE))])
        check(rc == 0 and "e2e_runner_timing_gate|mnist_mlp_compile"
              in out and "GATE gate_time_check: PASS" in out,
              "print-slow CLI: value line plus the shared verdict, got "
              "rc=%r out=%r" % (rc, out))
        rc, out = _cli(["print-slow", "ctest-Debug", "--table",
                        str(write_table(tmp, SCHEDULE_TABLE)),
                        "--keep-matched", "mnist_mlp_compile"])
        check(rc == 0 and "e2e_runner_timing_gate" in out
              and "mnist_mlp_compile|" not in out
              and "|mnist_mlp_compile" not in out,
              "print-slow CLI: --keep-matched must subtract, got rc=%r "
              "out=%r" % (rc, out))


def test_check_schedule_backfill_equivalence():
    # Guard six's ctest-side equivalence: hand the leg's -R/-E pair to
    # ctest --show-only as-is and the mapping-hit slow entries must be
    # in the output. A doc built from an -E that forgot the subtraction
    # shows mnist_mlp_compile swallowed by -E (ctest -E beats -R, F-12)
    # -- red naming the swallowed entry; the subtracted doc passes.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        doc_dir = Path(tmp) / "docs"
        doc_dir.mkdir()
        doc_bug = doc_dir / "bug.json"
        doc_bug.write_text(_show_only_doc(["plain_unit_entry"]),
                           encoding="utf-8")
        doc_fix = doc_dir / "fix.json"
        doc_fix.write_text(_show_only_doc(["mnist_mlp_compile",
                                           "plain_unit_entry"]),
                           encoding="utf-8")
        mapping = ["--config", "ctest-Debug",
                   "--mapping", "mnist_mlp_compile|plain_unit_entry"]
        rc, out = _schedule(table, families, root,
                            mapping + ["--show-only", str(doc_bug)])
        check(rc == 1 and "mnist_mlp_compile" in out
              and "GATE gate_time_check: FAIL" in out,
              "check-schedule: a mapped slow entry swallowed by -E must "
              "be named, got rc=%r out=%r" % (rc, out))
        rc, out = _schedule(table, families, root,
                            mapping + ["--show-only", str(doc_fix)])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "check-schedule: the subtracted pair must pass, got rc=%r "
              "out=%r" % (rc, out))


def test_check_schedule_phantom_glob():
    # Guard nine: every family src glob must hit at least one existing
    # repo file (D-7: the old draft's src/vm/native_tensor* was exactly
    # such a phantom), named in the finding.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        phantom_families = Path(tmp) / "ghost.txt"
        phantom_families.write_text(
            TENSOR_FAMILIES
            + "family ghost src src/ghostly_missing_xyz/**\n"
              "family ghost unit ghost_slow_entry\n", encoding="utf-8")
        rc, out = _schedule(table, phantom_families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 1 and "src/ghostly_missing_xyz/**" in out,
              "check-schedule: a phantom src glob must name the glob, "
              "got rc=%r out=%r" % (rc, out))
        (root / "src" / "ghostly_missing_xyz").mkdir()
        (root / "src" / "ghostly_missing_xyz" / "ghost.cpp").write_text(
            "// stub\n", encoding="utf-8")
        rc, out = _schedule(table, phantom_families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "check-schedule: once the glob hits a file the phantom "
              "finding must clear, got rc=%r out=%r" % (rc, out))


def test_check_schedule_over_slow_without_reason():
    # A row whose reference sits above SLOW_MIN_REF_S yet stays 常规 with
    # an empty reason column is an unexamined schedule decision: red.
    # Filling the reason (or dropping under 300 s) clears it.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        over = Path(tmp) / "over.txt"
        over.write_text(SCHEDULE_TABLE
                        + "ctest-Debug over_slow_plain 301.00 1 常规 "
                          "ctest-junit-serial -\n", encoding="utf-8")
        rc, out = _schedule(over, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 1 and "over_slow_plain" in out
              and "GATE gate_time_check: FAIL" in out,
              "check-schedule: a 301 s 常规 row with no reason must be "
              "named, got rc=%r out=%r" % (rc, out))
        over.write_text(SCHEDULE_TABLE
                        + "ctest-Debug over_slow_plain 301.00 1 常规 "
                          "ctest-junit-serial - every-round by ruling\n",
                        encoding="utf-8")
        rc, out = _schedule(over, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 0, "check-schedule: the filled reason must clear "
              "the finding, got rc=%r out=%r" % (rc, out))


def test_check_schedule_exception_rows_need_claim():
    # Guard six 甲/乙: every category!=常规 row must own a verifiable
    # recovery path -- a family whose unit/e2e fragment hits the entry
    # name (甲) or a non-empty reason column (乙). The Qt-shape case the
    # batch note pins: four 慢功能 rows with empty reasons go red, the
    # filled wording turns them green; the tensor-shape case needs no
    # reason because the family claims them (甲).
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        bare = Path(tmp) / "bare.txt"
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 慢功能 "
                          "ctest-junit-serial -\n", encoding="utf-8")
        rc, out = _schedule(bare, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 1 and "ghost_slow" in out,
              "check-schedule: a 慢功能 row with no reason and no family "
              "must be named, got rc=%r out=%r" % (rc, out))
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 慢功能 "
                          "ctest-junit-serial - covered by the IDE leg "
                          "every round\n", encoding="utf-8")
        rc, out = _schedule(bare, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 0, "check-schedule: the filled reason (乙) must "
              "clear the finding, got rc=%r out=%r" % (rc, out))
        # 甲 path: reason stays empty, but a family unit fragment hits
        # the name and the family has a src rule over a real file.
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 慢功能 "
                          "ctest-junit-serial -\n", encoding="utf-8")
        claim_families = Path(tmp) / "claim.txt"
        claim_families.write_text(
            TENSOR_FAMILIES + "family ghost src src/ghostly/x.cpp\n"
                              "family ghost unit ghost_slow\n",
            encoding="utf-8")
        (root / "src" / "ghostly").mkdir()
        (root / "src" / "ghostly" / "x.cpp").write_text("// stub\n",
                                                        encoding="utf-8")
        rc, out = _schedule(bare, claim_families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 0, "check-schedule: a family whose unit fragment "
              "hits the entry (甲) must clear the finding, got rc=%r "
              "out=%r" % (rc, out))


def test_derive_e2e_subset_set_formula():
    # The e2e side of D-7's 回补 formula: this round's rows are the
    # manifest lines that carry their own explicit 类别=常规 row under
    # the leg's config, unioned with the lines the mapping's e2e regex
    # re.search-hits, in manifest order, deduped. The default row prices
    # budgets for unlisted lines but never joins the subset (that would
    # pull the full 2600-line manifest into every T0 round -- the gain
    # comes from sharding, not from category filtering). A 演示验收 row
    # enters only through a mapping hit.
    with tempfile.TemporaryDirectory() as tmp:
        rows = load_table(write_table(tmp, SCHEDULE_TABLE))
        lines = E2E_MANIFEST.splitlines()
        subset = derive_e2e_subset(lines, rows, "e2e-Debug", "smoke_one")
        check(subset == ["smoke_one 0", "stdlib_stub_size 5"],
              "derive_e2e_subset: want the hit line then the explicit-"
              "常规 line in manifest order, got %r" % (subset,))
        check(derive_e2e_subset(lines, rows, "e2e-Debug", None)
              == ["stdlib_stub_size 5"],
              "derive_e2e_subset: with no mapping only the explicit-"
              "常规 line stays, got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug", None))
        check(derive_e2e_subset(lines, rows, "e2e-Debug",
                                "smoke_one|e2e_stub_demo")
              == ["smoke_one 0", "stdlib_stub_size 5",
                  "e2e_stub_demo 3"],
              "derive_e2e_subset: a mapping hit pulls the demo line "
              "back (backfill), got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug",
                                  "smoke_one|e2e_stub_demo"))
        manifest = Path(tmp) / "m.txt"
        manifest.write_text(E2E_MANIFEST, encoding="utf-8")
        rc, out = _cli(["derive-e2e-subset", "e2e-Debug", "--table",
                        str(write_table(tmp, SCHEDULE_TABLE)),
                        "--manifest", str(manifest),
                        "--mapping", "smoke_one"])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "derive-e2e-subset CLI: the subcommand must exist and "
              "pass, got rc=%r out=%r" % (rc, out))


def test_check_schedule_e2e_universe():
    # Guard six on the e2e universe: the 演示验收 manifest line must own
    # its claim (reason empty -> red), and --derived-e2e compares the
    # wiring's written subset file against the formula -- a subset that
    # dropped the explicit-常规 line is red.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        manifest = Path(tmp) / "manifest.txt"
        manifest.write_text(E2E_MANIFEST, encoding="utf-8")
        derived = Path(tmp) / "derived.txt"
        derived.write_text("smoke_one 0\nstdlib_stub_size 5\n",
                           encoding="utf-8")
        e2e_args = ["--config", "ctest-Debug", "--e2e-config",
                    "e2e-Debug", "--e2e-manifest", str(manifest),
                    "--e2e-mapping", "smoke_one", "--derived-e2e",
                    str(derived)]
        rc, out = _schedule(table, families, root, e2e_args)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "check-schedule: the claimed demo row and a matching "
              "derived subset must pass, got rc=%r out=%r" % (rc, out))
        derived.write_text("smoke_one 0\n", encoding="utf-8")
        rc, out = _schedule(table, families, root, e2e_args)
        check(rc == 1 and "stdlib_stub_size" in out,
              "check-schedule: a derived subset missing the explicit-"
              "常规 line must be named, got rc=%r out=%r" % (rc, out))
        bare_table = Path(tmp) / "bare_e2e.txt"
        bare_table.write_text(
            SCHEDULE_TABLE.replace(
                "e2e-Debug e2e_stub_demo 40.00 1 演示验收 "
                "e2e-runner-serial - demo class carried by T2",
                "e2e-Debug e2e_stub_demo 40.00 1 演示验收 "
                "e2e-runner-serial -"), encoding="utf-8")
        derived.write_text("smoke_one 0\nstdlib_stub_size 5\n",
                           encoding="utf-8")
        rc, out = _schedule(bare_table, families, root, e2e_args)
        check(rc == 1 and "e2e_stub_demo" in out,
              "check-schedule: an e2e 演示验收 row with no reason and no "
              "family must be named, got rc=%r out=%r" % (rc, out))


def run():
    test_slow_regex_per_config_and_keep_matched()
    test_check_schedule_backfill_equivalence()
    test_check_schedule_phantom_glob()
    test_check_schedule_over_slow_without_reason()
    test_check_schedule_exception_rows_need_claim()
    test_derive_e2e_subset_set_formula()
    test_check_schedule_e2e_universe()
