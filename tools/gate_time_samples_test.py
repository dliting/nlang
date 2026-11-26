# gate_time_samples_test.py -- samples scheduling-class cases for
# gate_time_schedule.py (the 2026-11-26 user ruling, docs/dev/
# gate_timing_design.md): a `samples` sentinel token schedules the
# demo-acceptance pair out of every round by class, not by seconds.
# The split out of gate_time_schedule_test.py is the source-size
# guard's demand (both modules stay under the file cap) -- the samples
# scenario owns its own stub corpus end to end, so it is a cohesion
# seam, and the aggregator entry point is unchanged (gate_time_
# check_test.py's main() calls run()). Stdlib only; run with -I.

import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_check import main as run_cli
from gate_time_table import load_table
from gate_time_testutil import check, write_table
from gate_time_schedule import (derive_e2e_subset, exclusion_regex,
                                sample_names, slow_names)

# Samples-class table (2026-11-26 user ruling): a `samples` sentinel
# token schedules the demo-acceptance pair out of every round by class,
# not by seconds. The pair reads fast on its baseline rows (0.44/10.01 s,
# under the slow gate) yet must join the exclusion union exactly like a
# slow member; slow_names stays untouched so guard six demands no
# recovery claim and the closeout carrier keeps pairing rows as before.
SAMPLES_TABLE = """\
# stub samples table
ctest-Debug default 100.00 1 ctest-junit-serial - stub
ctest-Debug plain_unit_entry 5.00 1 ctest-junit-serial - stub
ctest-Debug e2e_runner_timing_gate 409.77 1 ctest-junit-serial - timing probes carried every round elsewhere
ctest-Debug mnist_mlp_compile 0.44 1 ctest-junit-serial samples 演示验收:仅作samples,每轮判定腿排除,收口批排除关闭时承接
ctest-Debug mnist_mlp_run 10.01 1 ctest-junit-serial samples 演示验收:仅作samples,每轮判定腿排除,收口批排除关闭时承接
ctest-ASan mnist_mlp_run 36.56 1 ctest-junit-serial samples 演示验收:仅作samples,每轮判定腿排除,收口批排除关闭时承接
e2e-Debug default 100.00 1 e2e-runner-serial - stub
e2e-Debug e2e_stub_samples 40.00 1 e2e-runner-serial samples 演示验收:仅作samples,每轮判定腿排除
"""

SAMPLES_MANIFEST = """\
#stub samples manifest
e2e_stub_samples 3
plain_e2e_line 0
"""


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


def test_samples_mark_schedules_by_class_not_by_seconds():
    # 2026-11-26 user ruling: the demo-acceptance pair (mnist) goes back
    # to samples-only. The `samples` sentinel token is a scheduling class:
    # the entry joins every round's -E exclusion union regardless of its
    # seconds -- fast samples are excluded exactly like a slow member, and
    # the closeout batch re-runs them by running its legs with the
    # exclusions off. slow_names itself must stay untouched: guard six
    # demands no recovery claim for a samples member (its reason column
    # documents the class, not a 乙 claim), and guard eleven pairs the
    # ASan/Release rows in the closeout as before. --keep-matched
    # subtracts samples members too (F-12's shape: a leg mapping the
    # entry re-runs it), and the e2e formula drops samples lines the
    # same way it drops slow lines.
    with tempfile.TemporaryDirectory() as tmp:
        rows = load_table(write_table(tmp, SAMPLES_TABLE))
        check(set(sample_names(rows)) == {"mnist_mlp_compile",
                                         "mnist_mlp_run",
                                         "e2e_stub_samples"},
              "sample_names: the samples roster must read the sentinel "
              "column, got %r" % (sorted(sample_names(rows)),))
        check(set(slow_names(rows)) == {"e2e_runner_timing_gate"},
              "slow_names: a samples member must not leak into the slow "
              "set, got %r" % (sorted(slow_names(rows)),))
        regex = exclusion_regex(rows, "ctest-Debug")
        check(regex == "e2e_runner_timing_gate|e2e_stub_samples"
                       "|mnist_mlp_compile|mnist_mlp_run",
              "exclusion_regex: the union must carry the samples members "
              "alongside the slow set, got %r" % regex)
        check(exclusion_regex(rows, "ctest-Debug", "mnist_mlp")
              == "e2e_runner_timing_gate|e2e_stub_samples",
              "exclusion_regex: keep-matched must subtract the samples "
              "members it hits, got %r"
              % exclusion_regex(rows, "ctest-Debug", "mnist_mlp"))
        check(exclusion_regex(rows, "ctest-ASan") == regex,
              "exclusion_regex: the samples class is one global roster "
              "like the slow set, got %r"
              % exclusion_regex(rows, "ctest-ASan"))
        # The e2e formula: an explicit row under samples never joins the
        # round subset on its own, but a mapping hit pulls it back --
        # the same backfill rule slow lines follow (F-12: -R beats the
        # exclusion).
        lines = SAMPLES_MANIFEST.splitlines()
        check(derive_e2e_subset(lines, rows, "e2e-Debug", None) == [],
              "derive_e2e_subset: a samples line must not join the round "
              "subset, got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug", None))
        check(derive_e2e_subset(lines, rows, "e2e-Debug", "e2e_stub")
              == ["e2e_stub_samples 3"],
              "derive_e2e_subset: a mapping hit must pull the samples "
              "line back (F-12), got %r"
              % derive_e2e_subset(lines, rows, "e2e-Debug", "e2e_stub"))
        # check-schedule: no claim is demanded for a samples member --
        # the slow set holds only e2e_runner_timing_gate (covered by its
        # reason column), and the samples rows pass with their class
        # wording. An empty families file carries the 甲 path nowhere,
        # so the pass proves the exemption and not an accidental claim.
        table = write_table(tmp, SAMPLES_TABLE)
        families = Path(tmp) / "families.txt"
        families.write_text("# stub families\n", encoding="utf-8")
        root = Path(tmp) / "repo"
        root.mkdir()
        rc, out = _cli(["check-schedule", "--table", str(table),
                        "--families", str(families),
                        "--repo-root", str(root), "--config",
                        "ctest-Debug"])
        check(rc == 0 and "GATE gate_time_check: PASS" in out,
              "check-schedule: samples members must need no recovery "
              "claim, got rc=%r out=%r" % (rc, out))


def run():
    test_samples_mark_schedules_by_class_not_by_seconds()
