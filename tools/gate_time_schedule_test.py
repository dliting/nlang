# gate_time_schedule_test.py -- schedule-axis cases for
# gate_time_schedule.py (guard six and guard nine, docs/dev/
# gate_timing_design.md D-16/D-17): print-slow builds the -E regex from
# the one global slow set -- an entry name is slow exactly when its
# baseline row's reference seconds exceed SLOW_MIN_REF_S -- and
# subtracts whatever the leg's own -R mapping will run again (ctest -E
# beats -R, F-12); check-schedule pins the backfill equivalence ctest
# itself computes (a saved --show-only doc stands in for the leg), the
# 甲/乙 recovery path every row of a slow member must own, the phantom
# src glob (guard nine) and the e2e set formula "rows the leg's config
# lists that are not slow members, union mapping hits, manifest order,
# deduped". Stub tables, stub family files and saved ctest --show-only
# documents keep the corpus hermetic: no ctest run, no tree. Entry
# point: gate_time_check_test.py (its main() calls run()). Stdlib only;
# run with -I.

import io
import json
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_table import load_table
from gate_time_testutil import check, write_table
from gate_time_schedule import (derive_e2e_subset, exclusion_regex,
                                slow_names)

# Stub reference table for the schedule cases (D-16/D-17: the baseline
# row's reference seconds are the whole fast/slow input, no category
# column). ctest-Debug carries the classifying rows of three slow
# members (409.77 / 220.00 / 110.00 s, each above the SLOW_MIN_REF_S
# gate, every
# reason standing in for a 乙 claim), one fast row sitting one second
# under the gate with no claim of its own, and a `default` row whose 100
# s reference never joins the slow set (coarse rows do not classify).
# The ctest-ASan config prices a heavier build of one slow member and an
# 800 s sanitizer-only entry that never classifies (no row under
# ctest-Debug or ctest-IDE). The e2e-Debug rows show the other half of
# D-16: the leg's own config row never decides membership --
# e2e_runner_timing_gate is a slow member through its ctest-Debug row
# although its e2e row reads 50.00 s.
SCHEDULE_TABLE = """\
# stub schedule table
ctest-Debug default 100.00 1 ctest-junit-serial - stub
ctest-Debug plain_unit_entry 5.00 1 ctest-junit-serial - stub
ctest-Debug just_under_gate 59.00 1 ctest-junit-serial -
ctest-Debug e2e_runner_timing_gate 409.77 1 ctest-junit-serial - timing probes carried every round elsewhere
ctest-Debug mnist_mlp_compile 220.00 1 ctest-junit-serial - demo class carried by T2
ctest-Debug mainwindow_tests 110.00 1 ctest-junit-serial - Qt face carried by the IDE leg
ctest-IDE default 100.00 1 ctest-junit-serial - stub
ctest-IDE e2e_runner_timing_gate 409.80 1 ctest-junit-serial - timing probes carried every round elsewhere
ctest-IDE mnist_mlp_compile 240.00 1 ctest-junit-serial - demo class carried by T2
ctest-IDE mainwindow_tests 340.00 1 ctest-junit-serial - Qt face carried by the IDE leg
ctest-ASan default 100.00 1 ctest-junit-serial - stub
ctest-ASan mainwindow_tests 372.19 1 ctest-junit-serial - instrumented face carried by T2
ctest-ASan asan_heavy_entry 800.00 1 ctest-junit-serial - sanitizer-only entry
e2e-Debug default 100.00 1 e2e-runner-serial - stub
e2e-Debug stdlib_stub_size 30.00 1 e2e-runner-serial - stub reason
e2e-Debug e2e_stub_demo 40.00 1 e2e-runner-serial - demo class carried by T2
e2e-Debug e2e_runner_timing_gate 50.00 1 e2e-runner-serial - timing probes carried every round elsewhere
"""

# The tensor family as D-7 甲 specifies it: a src rule and a unit
# fragment that re.search-hits the slow entry name.
TENSOR_FAMILIES = """\
# stub families
family tensor src src/native/tensor/**
family tensor unit mnist_mlp_compile|mnist_mlp_run
"""

E2E_MANIFEST = """\
#stub manifest
smoke_one 0
stdlib_stub_size 5
e2e_runner_timing_gate 9
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


def test_exclusion_regex_is_the_one_global_set_and_keep_matched_subtracts():
    # print-slow: the joined -E regex is the ONE global slow set (sorted;
    # the same names for every config -- the config label never filters,
    # D-16), so the ctest-ASan and ctest-Debug legs subtract the same
    # names and only the mapped closeout legs re-run them. Membership
    # comes from the baseline row's seconds alone: the 59 s row under the
    # gate, the 800 s sanitizer-only entry (no baseline row) and the 100
    # s `default` rows all stay out. --keep-matched subtracts whatever
    # the leg's -R regex re.search-hits (ctest partial-match semantics),
    # so a mapped slow entry stays runnable (F-12). "." matches every
    # name, emptying the regex.
    with tempfile.TemporaryDirectory() as tmp:
        rows = load_table(write_table(tmp, SCHEDULE_TABLE))
        check(set(slow_names(rows)) == {"e2e_runner_timing_gate",
                                        "mainwindow_tests",
                                        "mnist_mlp_compile"},
              "slow_names: membership must come from the baseline row's "
              "seconds, got %r" % (sorted(slow_names(rows)),))
        regex = "e2e_runner_timing_gate|mainwindow_tests|mnist_mlp_compile"
        check(exclusion_regex(rows, "ctest-Debug") == regex,
              "exclusion_regex: want the sorted joined regex, got %r"
              % exclusion_regex(rows, "ctest-Debug"))
        check(exclusion_regex(rows, "ctest-ASan") == regex,
              "exclusion_regex: the config label must not filter the "
              "global set, got %r" % exclusion_regex(rows, "ctest-ASan"))
        check(exclusion_regex(rows, "ctest-Debug", "mnist_mlp_compile")
              == "e2e_runner_timing_gate|mainwindow_tests",
              "exclusion_regex: a keep-matched regex must subtract the "
              "hit entry (F-12 backfill), got %r"
              % exclusion_regex(rows, "ctest-Debug", "mnist_mlp_compile"))
        # ctest partial match: the mapping regex "plain" as a fragment
        # must hit nothing here, "mnist_mlp" must hit its entry.
        check(exclusion_regex(rows, "ctest-Debug", "mnist_mlp")
              == "e2e_runner_timing_gate|mainwindow_tests",
              "exclusion_regex: keep-matched is a partial match like "
              "ctest -R, got %r"
              % exclusion_regex(rows, "ctest-Debug", "mnist_mlp"))
        check(exclusion_regex(rows, "ctest-Debug", ".") == "",
              "exclusion_regex: a '.' mapping hits every entry, leaving "
              "no exclusion, got %r"
              % exclusion_regex(rows, "ctest-Debug", "."))
        # The wiring captures stdout, so the member roster (D-17: a bare
        # pattern is unreadable in a leg log) rides on stderr.
        err = io.StringIO()
        with redirect_stderr(err):
            rc, out = _cli(["print-slow", "ctest-Debug", "--table",
                            str(write_table(tmp, SCHEDULE_TABLE))])
        check(rc == 0 and out.startswith(regex + "\n")
              and "GATE gate_time_check: PASS" in out,
              "print-slow CLI: the value line plus the shared verdict, "
              "got rc=%r out=%r" % (rc, out))
        err_text = err.getvalue()
        check("excludes 3 members" in err_text
              and "mnist_mlp_compile" in err_text
              and regex not in err_text,
              "print-slow CLI: the member roster must ride on stderr, "
              "got %r" % err_text)
        rc, out = _cli(["print-slow", "ctest-Debug", "--table",
                        str(write_table(tmp, SCHEDULE_TABLE)),
                        "--keep-matched", "mnist_mlp_compile"])
        check(rc == 0 and "e2e_runner_timing_gate|mainwindow_tests"
              in out and "mnist_mlp_compile" not in out,
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


def test_check_schedule_slow_member_needs_claim():
    # Guard six 甲/乙: every row of a slow member (D-16: baseline row
    # above SLOW_MIN_REF_S) must own a verifiable recovery path -- a
    # family whose unit/e2e fragment hits the entry name (甲) or a
    # non-empty reason column naming where the entry still runs (乙).
    # The 99 s ghost is the case the deleted 300 s category threshold
    # left exempt and D-16 catches; the base table's 59 s row shows the
    # strict boundary from the other side (above the gate, but one
    # second below, needs no claim at all). The Qt-shape case the batch
    # note pins: a slow row with an empty reason goes red, the filled
    # wording turns it green; the tensor-shape family clears the same
    # finding through 甲 with the reason still empty.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        bare = Path(tmp) / "bare.txt"
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 "
                          "ctest-junit-serial -\n", encoding="utf-8")
        rc, out = _schedule(bare, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 1 and "ghost_slow" in out
              and "just_under_gate" not in out,
              "check-schedule: a 99 s slow member with no reason and no "
              "family must be named (and the 59 s fast row must not), "
              "got rc=%r out=%r" % (rc, out))
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 "
                          "ctest-junit-serial - covered by the IDE leg "
                          "every round\n", encoding="utf-8")
        rc, out = _schedule(bare, families, root,
                            ["--config", "ctest-Debug"])
        check(rc == 0, "check-schedule: the filled reason (乙) must "
              "clear the finding, got rc=%r out=%r" % (rc, out))
        # 甲 path: reason stays empty, but a family unit fragment hits
        # the name and the family has a src rule over a real file.
        bare.write_text(SCHEDULE_TABLE
                        + "ctest-Debug ghost_slow 99.00 1 "
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
    # The e2e side of D-7's 回补 formula (R-64): this round's rows are
    # the manifest lines the leg's config lists explicitly AND that are
    # not slow members, unioned with the lines the mapping's e2e regex
    # re.search-hits, in manifest order, deduped. Membership never comes
    # from this leg's own row: e2e_runner_timing_gate reads 50.00 s
    # under e2e-Debug yet stays out because its ctest-Debug baseline row
    # says slow (D-16). The default row prices budgets for unlisted
    # lines but never joins the subset (that would pull the full 2600-
    # line manifest into every T0 round -- the gain comes from sharding,
    # not from filtering). A slow line re-enters only through a mapping
    # hit.
    with tempfile.TemporaryDirectory() as tmp:
        rows = load_table(write_table(tmp, SCHEDULE_TABLE))
        lines = E2E_MANIFEST.splitlines()
        subset = derive_e2e_subset(lines, rows, "e2e-Debug", "smoke_one")
        check(subset == ["smoke_one 0", "stdlib_stub_size 5",
                         "e2e_stub_demo 3"],
              "derive_e2e_subset: want the hit line then the explicit "
              "fast lines in manifest order, got %r" % (subset,))
        check(derive_e2e_subset(lines, rows, "e2e-Debug", None)
              == ["stdlib_stub_size 5", "e2e_stub_demo 3"],
              "derive_e2e_subset: with no mapping only the explicit fast "
              "lines stay, got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug", None))
        check(derive_e2e_subset(lines, rows, "e2e-Debug",
                                "smoke_one|e2e_runner")
              == ["smoke_one 0", "stdlib_stub_size 5",
                  "e2e_runner_timing_gate 9", "e2e_stub_demo 3"],
              "derive_e2e_subset: a mapping hit pulls the slow line "
              "back (backfill), got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug",
                                  "smoke_one|e2e_runner"))
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
    # Guard six on the e2e universe: every row of a slow member must own
    # its claim (the e2e row of e2e_runner_timing_gate with an empty
    # reason goes red even though its own seconds read fast), while a
    # FAST row's empty reason needs no claim at all -- the 乙 rule fires
    # on membership, not on seconds. --derived-e2e compares the wiring's
    # written subset file against the formula: a dropped explicit fast
    # line and an unmapped slow line both go red naming the line.
    with tempfile.TemporaryDirectory() as tmp:
        table, families, root = _fixture(tmp)
        manifest = Path(tmp) / "manifest.txt"
        manifest.write_text(E2E_MANIFEST, encoding="utf-8")
        derived = Path(tmp) / "derived.txt"
        derived.write_text("smoke_one 0\nstdlib_stub_size 5\n"
                           "e2e_stub_demo 3\n", encoding="utf-8")
        e2e_args = ["--config", "ctest-Debug", "--e2e-config",
                    "e2e-Debug", "--e2e-manifest", str(manifest),
                    "--e2e-mapping", "smoke_one", "--derived-e2e",
                    str(derived)]
        rc, out = _schedule(table, families, root, e2e_args)
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "check-schedule: the claimed slow rows and a matching "
              "derived subset must pass, got rc=%r out=%r" % (rc, out))
        derived.write_text("smoke_one 0\ne2e_stub_demo 3\n",
                           encoding="utf-8")
        rc, out = _schedule(table, families, root, e2e_args)
        check(rc == 1 and "stdlib_stub_size" in out,
              "check-schedule: a derived subset missing the explicit "
              "fast line must be named, got rc=%r out=%r" % (rc, out))
        derived.write_text("smoke_one 0\nstdlib_stub_size 5\n"
                           "e2e_runner_timing_gate 9\n",
                           encoding="utf-8")
        rc, out = _schedule(table, families, root, e2e_args)
        check(rc == 1 and "e2e_runner_timing_gate 9" in out,
              "check-schedule: a derived subset holding an unmapped "
              "slow line must be named, got rc=%r out=%r" % (rc, out))
        bare_table = Path(tmp) / "bare_e2e.txt"
        bare_table.write_text(
            SCHEDULE_TABLE.replace(
                "e2e-Debug e2e_runner_timing_gate 50.00 1 "
                "e2e-runner-serial - timing probes carried every round "
                "elsewhere",
                "e2e-Debug e2e_runner_timing_gate 50.00 1 "
                "e2e-runner-serial -"), encoding="utf-8")
        derived.write_text("smoke_one 0\nstdlib_stub_size 5\n"
                           "e2e_stub_demo 3\n", encoding="utf-8")
        rc, out = _schedule(bare_table, families, root, e2e_args)
        check(rc == 1 and "e2e_runner_timing_gate" in out,
              "check-schedule: a slow member's e2e row with no reason "
              "and no family must be named, got rc=%r out=%r"
              % (rc, out))
        fast_bare = Path(tmp) / "fast_bare.txt"
        fast_bare.write_text(
            SCHEDULE_TABLE.replace(
                "e2e-Debug e2e_stub_demo 40.00 1 e2e-runner-serial "
                "- demo class carried by T2",
                "e2e-Debug e2e_stub_demo 40.00 1 e2e-runner-serial -"),
            encoding="utf-8")
        rc, out = _schedule(fast_bare, families, root, e2e_args)
        check(rc == 0, "check-schedule: a FAST row with an empty reason "
              "needs no claim, got rc=%r out=%r" % (rc, out))


def run():
    test_exclusion_regex_is_the_one_global_set_and_keep_matched_subtracts()
    test_check_schedule_backfill_equivalence()
    test_check_schedule_phantom_glob()
    test_check_schedule_slow_member_needs_claim()
    test_derive_e2e_subset_set_formula()
    test_check_schedule_e2e_universe()
