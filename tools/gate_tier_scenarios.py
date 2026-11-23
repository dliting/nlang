# gate_tier_scenarios.py -- the rounds that follow the first full
# T1 batch in the tier smoke (entry: gate_tier_test.py, sandbox:
# gate_tier_sandbox.build_sandbox): the mapped T0 round (the
# tensor change comes back through -R, F-12), the unmapped
# conservative T0 round (R-53) and the T1/T2 dry-run listings with
# the leg records. Each function launches its own batch against the
# shared sandbox and appends findings to gate_tier_sandbox's
# failures list. Stdlib only; run with -I.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_tier_sandbox import check, run_tier  # pyright: ignore[reportMissingImports]


def run_mapped_round(bash, sandbox):
    gates_dir = sandbox / "temp" / "tier" / "gates"
    tree_abs = {name: (sandbox / "trees" / name).as_posix()
                for name in ("master", "asan", "ide")}
    # T0 through the schedule axis: the tensor src change maps the
    # mnist entries back in via -R (F-12 backfill), so the -E keeps
    # only the unmapped slow name; the e2e selection runs through
    # derive-e2e-subset instead of a bare grep.
    t0 = run_tier(bash, sandbox, "T0",
                  "--master-tree",
                  (sandbox / "trees" / "master").as_posix(),
                  "--paths", "src/native/tensor/x.cpp")
    t0_out = t0.stdout + t0.stderr
    check(t0.returncode == 0,
          f"T0 batch exited rc={t0.returncode}; output:\n{t0_out}")
    for leg in ("T0-unit", "T0-unit-budget", "T0-unit-timeouts",
                "T0-e2e"):
        check(f"GATE {leg}: PASS" in t0_out,
              f"no PASS verdict for {leg}; output:\n{t0_out}")
    t0_unit = gates_dir / "T0-unit.invocations"
    if t0_unit.exists():
        body = t0_unit.read_text()
        check("-R 'mnist_mlp_compile|mnist_mlp_run'" in body,
              "T0-unit lost the mapped unit selector: "
              f"{body!r}")
        check("--claim" not in body,
              "the master tree is not IDE-built, so no T0 leg may "
              f"declare the nide-gui claim: {body!r}")
        check("-E 'e2e_runner_timing_gate|mainwindow_tests|"
              "perf_stub_compile|perf_stub_run'" in body,
              "T0-unit must keep only the unmapped slow names (the "
              "mapped mnist name came back through -R) plus the "
              f"perf set in -E: {body!r}")
    else:
        check(False, "stub gate.sh never received T0-unit")
    subset = sandbox / "temp" / "tier" / "t0_subset.txt"
    check(subset.exists(), "T0 derived no e2e subset file")
    if subset.exists():
        lines = [line for line in subset.read_text(
            encoding="utf-8").splitlines() if line.strip()]
        check(lines == ["smoke_one 42", "stdlib_stub_size 5"],
              "the derived e2e subset must be the mapping hits "
              "∪ the lines with their own explicit non-default row "
              "outside the slow set, in "
              f"manifest order, got {lines!r}")
    # Task 12: the guard legs ride the scheduled round too, and the
    # mapped round additionally wires guard six's run-set channel
    # (the leg's own -R/-E replayed through --show-only) and guard
    # nine's derived-subset equality into T-schedule.
    for leg_name in ("T-guards-complete", "T-guards-orphans",
                     "T-schedule"):
        check(f"GATE {leg_name}: PASS" in t0_out,
              f"T0 lost its {leg_name} leg; output:\n{t0_out}")
    t0_sched = gates_dir / "T-schedule.invocations"
    if t0_sched.exists():
        lines = [ln for ln in t0_sched.read_text(
            encoding="utf-8").splitlines()
            if "check-schedule" in ln]
        check(len(lines) >= 2,
              "the mapped T0 batch never added its own T-schedule "
              f"invocation: {lines!r}")
        if len(lines) >= 2:
            sched_line = lines[-1]
            for token in ("--mapping mnist_mlp_compile|mnist_mlp_run",
                          "--e2e-mapping smoke_", "--derived-e2e",
                          "t0_subset.txt"):
                check(token in sched_line,
                      f"the mapped T0 schedule guard lost "
                      f"{token!r}: {sched_line!r}")
            # The run-set channel is either the replayed --show-only
            # enumeration or the NAMED --tree fallback the assembly
            # prints when its own enumeration fails (a .bat door
            # cannot carry the -R/-E regex pipes through cmd -- what
            # must never happen is a mapped round with no run-set
            # channel at all, guard six then judges nothing).
            replay = ("--show-only" in sched_line
                      and "t0_runset.json" in sched_line)
            fallback = ("--tree " + tree_abs["master"]
                        in sched_line)
            check(replay or fallback,
                  "the mapped T0 schedule guard lost its run-set "
                  "channel (neither the --show-only replay nor the "
                  f"named --tree fallback): {sched_line!r}")
    else:
        check(False, "stub gate.sh never received T-schedule in "
              "the mapped T0 round")


def run_conservative_round(bash, sandbox):
    gates_dir = sandbox / "temp" / "tier" / "gates"
    t0_sched = gates_dir / "T-schedule.invocations"
    # R-53: the unmapped conservative round. test_impact.py answers an
    # unmapped path with rc=3 and no families, so the -R selector
    # becomes "." (the full Debug unit suite) -- but "." must NOT be
    # handed to print-slow as --keep-matched either: re.search hits
    # every slow entry, the subtraction empties the -E down to the
    # perf set alone, and the slow entries run inside the fast round
    # (the 466 s timing gate with its load-induced false red). The
    # unmapped branch therefore passes no --keep-matched at all: the
    # conservative round excludes the WHOLE slow set, the same
    # exclusion the full-scan T1 unit legs carry.
    t0c = run_tier(bash, sandbox, "T0",
                   "--master-tree",
                   (sandbox / "trees" / "master").as_posix(),
                   "--paths", "docs/dev/README.md")
    t0c_out = t0c.stdout + t0c.stderr
    check(t0c.returncode == 0,
          f"unmapped T0 batch exited rc={t0c.returncode}; "
          f"output:\n{t0c_out}")
    t0c_unit = gates_dir / "T0-unit.invocations"
    if t0c_unit.exists():
        body = t0c_unit.read_text()
        check("-R '.'" in body,
              "the conservative round must select everything with "
              f"-R '.': {body!r}")
        check("-E 'e2e_runner_timing_gate|mainwindow_tests|"
              "mnist_mlp_compile|perf_stub_compile|perf_stub_run'"
              in body,
              "the conservative round must exclude all three stub-"
              "table slow names plus the perf set (nothing is "
              "mapped back, so print-slow runs without --keep-"
              f"matched and keeps the whole slow set): {body!r}")
    else:
        check(False, "stub gate.sh never received the conservative "
              "T0-unit batch leg")
    # The conservative round feeds guard six no mapping at all (the
    # R-53 lesson: "." re.search-hits every slow entry and would
    # judge the backfill half vacuously), so its T-schedule line
    # carries neither --mapping nor an e2e mapping.
    if t0_sched.exists():
        lines = [ln for ln in t0_sched.read_text(
            encoding="utf-8").splitlines()
            if "check-schedule" in ln]
        check(len(lines) >= 3,
              "the conservative T0 batch never added its own "
              f"T-schedule invocation: {lines!r}")
        if len(lines) >= 3:
            check("--mapping" not in lines[-1],
                  "the conservative round must not hand guard six a "
                  f'"." mapping: {lines[-1]!r}')
            check("--e2e-mapping" not in lines[-1],
                  "the conservative round's e2e regex is empty, so "
                  "its schedule guard must not carry an e2e "
                  f"mapping either: {lines[-1]!r}")


def run_listing_rounds(bash, sandbox):
    gates_dir = sandbox / "temp" / "tier" / "gates"
    tree_abs = {name: (sandbox / "trees" / name).as_posix()
                for name in ("master", "asan", "ide")}
    temp_abs = (sandbox / "tests" / "temp").as_posix()
    tier_work = str(sandbox / "temp" / "tier")
    trees = ["--master-tree", "trees/master",
             "--asan-tree", "trees/asan",
             "--ide-tree", "trees/ide"]
    # Tier reuse: T2's copies of the shared ctest legs must NOT carry
    # the slow-set -E (the close-out sweep covers those entries),
    # while T1's own assembly does (t1_legs' slow_e parameter). The
    # perf names join EVERY ctest leg's -E either way (D-6: a
    # scheduled round may only run under the OFF-cache registration).
    # A dry run assembles the batch; it must not clear the work
    # directory the way an executed round does (R-106): a listing pass
    # over a work directory that still owes a record round would
    # otherwise destroy that round's junit evidence. Pre-place one
    # sentinel per clear site (ctest_leg's junit for every ctest leg,
    # the perf branch's T2-perf.xml) and require both to survive both
    # listings.
    gates_dir.mkdir(parents=True, exist_ok=True)
    pending = (gates_dir / "T1-ide-ctest.xml", gates_dir / "T2-perf.xml")
    for stale in pending:
        stale.write_text("pending-evidence\n", encoding="utf-8")
    dry1 = run_tier(bash, sandbox, "T1", *trees, "--dry-run")
    dry2 = run_tier(bash, sandbox, "T2", *trees, "--dry-run")
    for name, proc_dry in (("T1", dry1), ("T2", dry2)):
        check(proc_dry.returncode == 0,
              f"{name} --dry-run exited rc={proc_dry.returncode}; "
              f"output:\n{proc_dry.stdout + proc_dry.stderr}")
    for stale in pending:
        check(stale.exists() and stale.read_text(encoding="utf-8")
              == "pending-evidence\n",
              f"a --dry-run round destroyed the pending junit "
              f"{stale.name} in the work directory: the listing pass "
              "must assemble without clearing an executed round's "
              "evidence (R-106)")
    check("-E 'e2e_runner_timing_gate|mainwindow_tests|"
          "mnist_mlp_compile|perf_stub_compile|perf_stub_run'"
          in dry1.stdout,
          "the T1 dry run lost the slow+perf -E from its ctest "
          f"legs: {dry1.stdout!r}")
    # The dry-run listing must show the declarations verbatim at the
    # head of each sweep leg line (the operator reads the same line
    # the carrier guard parses): --gate-env on every ctest/e2e
    # sweep leg, --claim only on the IDE ctest leg.
    def dry_leg_line_from(proc_dry, name):
        hits = [ln for ln in proc_dry.stdout.splitlines()
                if ln.startswith(f"LEG {name}: ")]
        return hits[0] if len(hits) == 1 else ""
    for name, want_dir in (("T1-debug-unit", tree_abs["master"]),
                           ("T1-ide-ctest", tree_abs["ide"]),
                           ("T1-asan-unit", tree_abs["asan"]),
                           ("T1-debug-e2e", temp_abs)):
        line = dry_leg_line_from(dry1, name)
        check(f"--gate-env NLANG_GATE_WRITE_DIR={want_dir}" in line,
              f"the {name} dry-run leg line lost its --gate-env "
              f"declaration: {line!r}")
    check("--claim nide-gui"
          in dry_leg_line_from(dry1, "T1-ide-ctest"),
          "the IDE ctest dry-run leg line lost its nide-gui claim: "
          f"{dry_leg_line_from(dry1, 'T1-ide-ctest')!r}")
    check("--claim" not in dry_leg_line_from(dry1, "T1-debug-unit"),
          "a plain-tree leg must not declare the nide-gui claim: "
          f"{dry_leg_line_from(dry1, 'T1-debug-unit')!r}")
    # Task 12: the guard legs' shape is pinned from the dry-run
    # listing as well -- the operator reads that line, and an
    # invocations-only pin would pass on any stub that only echoes.
    # The coverage pair names all three per-tree pins and the perf
    # differential; the schedule core names both judged configs; no
    # guard leg carries a lock declaration (they read only).
    for gname, core in (("T-guards-complete", "check-complete"),
                        ("T-guards-orphans", "check-orphans")):
        gline = dry_leg_line_from(dry1, gname)
        for token in (core,
                      "--enum ctest-Debug=" + tier_work
                      + "/guard_enum_ctest-Debug.txt",
                      "--enum ctest-IDE=" + tier_work
                      + "/guard_enum_ctest-IDE.txt",
                      "--enum ctest-ASan=" + tier_work
                      + "/guard_enum_ctest-ASan.txt",
                      "--perf-names " + tier_work
                      + "/perf_names.txt"):
            check(token in gline,
                  f"the {gname} dry-run leg line lost {token!r}: "
                  f"{gline!r}")
        check("--gate-env" not in gline and "--claim" not in gline,
              f"read-only guard legs carry no lock declaration: "
              f"{gline!r}")
    gsched = dry_leg_line_from(dry1, "T-schedule")
    for token in ("check-schedule", "--config ctest-Debug",
                  "--perf-names", "--e2e-config e2e-Debug",
                  "--e2e-manifest"):
        check(token in gsched,
              f"the T-schedule dry-run leg line lost {token!r}: "
              f"{gsched!r}")
    check("--mapping" not in gsched and "--gate-env" not in gsched,
          f"T1's schedule guard is the no-mapping full-round form "
          f"with no lock declaration: {gsched!r}")
    reuse = [line for line in dry2.stdout.splitlines()
             if line.startswith("LEG T1-debug-unit: ")]
    check(len(reuse) == 1 and "e2e_runner_timing_gate" not in reuse[0]
          and "-E 'perf_stub_compile|perf_stub_run'" in reuse[0],
          "T2 must reuse the shared legs without the slow-set -E "
          "but with the perf-set -E: "
          f"{reuse!r}")
    # R-101 subset absorption: the close-out adds T2-asan-e2e-full, a
    # sweep of the same ASan tree over the whole e2e manifest, and the
    # subset manifest is a grep filter of that same file, so running
    # the subset leg too would repeat a time-contained part of the
    # full leg. The assembly drops it exactly when the full leg rides
    # the batch; the plain T1 round has no full sweep there and keeps
    # the subset leg.
    t2_subset = dry_leg_line_from(dry2, "T1-asan-e2e-subset")
    check(t2_subset == "",
          "T2 absorbed nothing: the ASan e2e subset leg still rides "
          f"the close-out listing: {t2_subset!r}")
    t1_subset = dry_leg_line_from(dry1, "T1-asan-e2e-subset")
    check(t1_subset != "" and "T2-asan-e2e-full" not in dry1.stdout,
          "T1 keeps the ASan e2e subset leg (no full sweep there): "
          f"{t1_subset!r}")

    # The perf class (D-6): its own T2 leg, the registration cache
    # flipped ON for the run, judged under -C Release, no -E of its
    # own (F-12: -E would cancel its -R). A restore leg closes the
    # ON-cache window after the group's post-hoc entries judged the
    # ON state -- batches run every remaining leg regardless of
    # reds, so the restore happens even on a red perf leg.
    # T2-carrier then pairs every exception row with its carrier
    # leg (guard eleven) from the batch's own leg_commands.tsv.
    def dry_leg_line(name):
        hits = [ln for ln in dry2.stdout.splitlines()
                if ln.startswith(f"LEG {name}: ")]
        return hits[0] if hits else ""
    perf_line = dry_leg_line("T2-perf")
    check("-DNLANG_PERF_TESTS=ON" in perf_line
          and "-C Release" in perf_line
          and "--output-on-failure" in perf_line
          and "-R '^(perf_stub_compile|perf_stub_run)$'" in perf_line
          and "-E '" not in perf_line,
          "the perf leg must flip the registration ON, run under "
          "-C Release with --output-on-failure (R-59), and carry "
          f"no -E of its own: {perf_line!r}")
    check("-DNLANG_PERF_TESTS=OFF" in dry_leg_line("T2-perf-restore"),
          "no restore leg closing the ON-cache window")
    # The three legs that name the sweep tree only INSIDE a single
    # bash -c string (cmake --build, cmake -B): gate.sh derives write
    # directories by walking argv words and cannot drill a quoted
    # script, so with the machine lock gone (D-10) these legs would
    # run unlocked against the Debug decision legs on the same tree
    # unless the assembly declares the directory at the leg layer.
    for leg_name in ("T2-release-build", "T2-perf", "T2-perf-restore"):
        line = dry_leg_line(leg_name)
        check(f"--gate-env NLANG_GATE_WRITE_DIR={tree_abs['master']}"
              in line,
              f"the {leg_name} dry-run leg line lost its --gate-env "
              f"write-directory declaration: {line!r}")
    carrier_line = dry_leg_line("T2-carrier")
    check("check-carrier" in carrier_line and "--leg-commands"
          in carrier_line and "--perf-names" in carrier_line,
          f"T2-carrier leg lost the pairing checker: {carrier_line!r}")
    # T2 inherits the guard legs through t1_legs (the close-out
    # judges the same coverage and schedule axes); they keep the no-
    # mapping full-round form there too.
    for gname in ("T-guards-complete", "T-guards-orphans",
                  "T-schedule"):
        gline = dry_leg_line(gname)
        check(gline != "",
              f"the T2 dry listing lost the {gname} guard leg — "
              "t1_legs must emit it for the close-out too")
        check("--mapping" not in gline,
              f"T2's {gname} leg must not carry a fast-round "
              f"mapping: {gline!r}")
    for leg_name, want in (("T2-asan-e2e-full", "--config=ASan"),
                           ("T2-release-e2e", "--config=Release")):
        line = dry_leg_line(leg_name)
        check(want in line,
              f"the {leg_name} leg must spell out its judging "
              f"--config VALUE (a path prefix cannot pair a config): "
              f"{line!r}")
    rec = gates_dir / "leg_commands.tsv"
    check(rec.exists(), "leg commands were never recorded for the "
          "carrier guard")
    if rec.exists():
        body = rec.read_text(encoding="utf-8")
        check("T2-perf\t--gate-env NLANG_GATE_WRITE_DIR="
              + tree_abs["master"] + " env" in body
              and "T2-perf-restore\t" in body
              and "T0-unit\t" not in body,
              "leg_commands.tsv must hold this batch's commands and "
              f"nothing an earlier batch recorded: {body!r}")
        # The declaration is part of the recorded line (the carrier
        # guard and the assembly pins read it there, and a leg
        # re-run from the record reproduces the same locks): the
        # sweep legs start with --gate-env, and the IDE one spells
        # the claim right behind it.
        check("T1-ide-ctest\t--gate-env NLANG_GATE_WRITE_DIR="
              + tree_abs["ide"] + " --claim nide-gui" in body
              and "T1-debug-unit\t--gate-env NLANG_GATE_WRITE_DIR="
              + tree_abs["master"] in body,
              "leg_commands.tsv lost the declaration head of the "
              f"sweep legs: {body!r}")
        check("T1-asan-e2e-subset\t" not in body,
              "the absorbed close-out batch must not record a "
              f"command for the subset leg either: {body!r}")

