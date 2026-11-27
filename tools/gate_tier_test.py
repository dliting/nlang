# gate_tier_test.py -- end-to-end smoke for the leg dispatch path.
# Pins the launch wiring of gate_tier.sh: every leg must actually
# reach tools/gate.sh. A stub gate.sh stands in for the real one
# (records the command it received, prints a PASS verdict), so the
# test exercises the tier assembly in a sandbox without running any
# real suite. A quoting bug that fuses "bash <path>" into one
# command word shows up here as rc=127 legs and NO-VERDICT lines.
# The sandbox fixture and the shared verdict state live in
# gate_tier_sandbox; the mapped/conservative T0 rounds and the
# dry-run listings live in gate_tier_scenarios; this file stays the
# single registered entry and runs the full-scan T1 batch itself.
# Run: python gate_tier_test.py
# Verdict line: "GATE gate_tier_smoke: PASS" (or FAIL), exit code
# follows.

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_tier_sandbox import (EXPECTED_LEGS, build_sandbox, check,  # pyright: ignore[reportMissingImports]
                               failures, find_git_bash, run_tier)
from gate_tier_scenarios import (run_conservative_round,  # pyright: ignore[reportMissingImports]
                                 run_listing_rounds, run_mapped_round)


def run_smoke():
    bash = find_git_bash()
    check(bash is not None, "no Git Bash found (MSYSTEM probe failed "
          "for every candidate); set NLANG_GIT_BASH")
    if bash is None:
        return False
    sandbox = Path(tempfile.mkdtemp(prefix="gate-tier-smoke-"))
    try:
        build_sandbox(sandbox)

        # The value producers own their machine-readable channel's line
        # endings (Task 12, run-set replay contract): bash command
        # substitution KEEPS a carriage return, and a CR riding into a
        # ctest -R/-E argv silently changes the regex (a CR-anchored
        # last alternative matches no entry name -- measured: -R
        # 'native_fs' selects an entry, -R 'native_fs<CR>' selects
        # none). Today the tier's MSYS grep/awk happen to strip the CR
        # as text-mode intermediaries, but that cleanness is an
        # accident of the pipeline, not a contract a door or a POSIX
        # awk would keep. Verified on RAW bytes with no shell in
        # between (subprocess pipes), because that is the channel the
        # tier actually captures.
        for value_probe in (
            ("gate_time_check.py", ["print-slow", "ctest-Debug",
                                    "--table",
                                    "tests/gate_time_reference.txt"]),
            ("test_impact.py", ["tools/test_families.txt",
                                "src/native/tensor/x.cpp"]),
        ):
            probe = subprocess.run(
                [sys.executable, "-I",
                 str(sandbox / "tools" / value_probe[0]),
                 *value_probe[1]],
                capture_output=True, cwd=sandbox)
            check(probe.returncode == 0,
                  f"the value probe {value_probe[0]} exited "
                  f"rc={probe.returncode}: {probe.stderr[:200]!r}")
            check(b"\r" not in probe.stdout,
                  f"the stdout value channel of {value_probe[0]} emits "
                  f"a carriage return (it would ride into ctest -R/-E "
                  f"argv when no text-mode filter sits in between): "
                  f"{probe.stdout[:200]!r}")

        #Plant stale flat artifacts from a "previous generation" in the
        #shared gates root: a same-named one and a differently-cased one
        #(the exact shape that once masqueraded as a fresh verdict). The
        #run-scoped layout must leave both untouched and read neither.
        gates_root = sandbox / "temp" / "tier" / "gates"
        gates_root.mkdir(parents=True)
        stale_same_name = gates_root / "T1-debug-unit.invocations"
        stale_same_name.write_text("stale-from-another-generation\n")
        stale_other_case = gates_root / "t1_debug_unit.gate"
        stale_other_case.write_text("GATE T1-debug-unit: PASS (stale)\n")

        # NLANG_GATE_MACHINE_SCALE (inside run_tier) pins the calibration
        # (D-4): the stub trees hold no suite, so an unpinned run would
        # burn live sampling rounds and fail on calibration, not wiring.
        # The trees are given relative to the run directory while every
        # pin below reads the absolute form: the tier absolutizes tree
        # arguments, because the e2e runner works from tests/e2e and a
        # relative tool path there resolves against the wrong directory.
        trees = ["--master-tree", "trees/master",
                 "--asan-tree", "trees/asan",
                 "--ide-tree", "trees/ide"]
        # A live batch exports NLANG_GATE_TIER_WORK and this smoke runs as
        # a ctest entry inside one: a nested batch must pin its own WORK.
        os.environ["NLANG_GATE_TIER_WORK"] = str(sandbox / "leaked-work")
        # --docs rides the batch so the docs legs' cross-language build/
        # check link order is pinned below; the stub gate.sh records the
        # leg commands without running them, so the docs payload costs
        # the smoke its assembly pass only.
        proc = run_tier(bash, sandbox, "T1", *trees, "--docs")
        output = proc.stdout + proc.stderr

        check(proc.returncode == 0,
              f"tier batch exited rc={proc.returncode}; output:\n{output}")
        check("rc=127" not in output, "rc=127 leg launch: command word fused")
        for leg in EXPECTED_LEGS:
            check(f"GATE {leg}: PASS" in output,
                  f"no PASS verdict for {leg} (see batch output above)")
        #Cross-language link checks read the sibling site: every build
        #must precede the first check, or a clean work dir cannot pass
        #check-en (its pages link into the zh tree built later).
        build_zh_at = output.find("GATE T-docs-build-zh: PASS")
        check_en_at = output.find("GATE T-docs-check-en: PASS")
        check(build_zh_at != -1 and check_en_at != -1 and
              build_zh_at < check_en_at,
              "docs legs ordered build/check per language - check-en ran "
              "before build-zh, so a clean work dir has no zh site to "
              "link-check against")
        runs = sorted(d for d in gates_root.iterdir() if d.is_dir())
        check(len(runs) == 1,
              f"expected exactly one run-scoped log directory, found "
              f"{[d.name for d in runs]} (flat artifacts must not appear)")
        #Windows pathlib globs are case-insensitive, so the planted
        #t1_debug_unit.gate would match a T1-* glob; enumerate instead
        #and demand the flat root holds nothing but the stale plants.
        #The stray list is built before the f-string: an expression
        #spanning lines inside one is Python 3.12+ grammar, and the
        #ctest entry runs this file under whichever Python3 the tree
        #resolved (3.9 on a bare miniconda configure).
        flat_files = {p.name for p in gates_root.iterdir() if p.is_file()}
        strays = sorted(flat_files - {stale_same_name.name,
                                      stale_other_case.name})
        check(flat_files == {stale_same_name.name, stale_other_case.name},
              f"leg artifacts landed flat in the gates root ({strays}) — "
              "verdict files must live inside the per-run directory")
        check(stale_same_name.read_text() == "stale-from-another-generation\n"
              and stale_other_case.read_text() == "GATE T1-debug-unit: PASS (stale)\n",
              "the batch overwrote or consumed a stale flat artifact — "
              "generations must be isolated by construction")
        if len(runs) != 1:
            return False
        run_dir = runs[0]
        check((run_dir / "batch.info").exists(),
              "no batch.info provenance file in the run directory")
        unit_invocations = run_dir / "T1-debug-unit.invocations"
        check(unit_invocations.exists(),
              "stub gate.sh never received T1-debug-unit — dispatch broken")
        if unit_invocations.exists():
            body = unit_invocations.read_text()
            check("ctest -C Debug" in body,
                  f"stub got unexpected command for T1-debug-unit: {body!r}")
            check("gate.sh" not in body,
                  "command word fused with the launcher path "
                  f"(quoting bug): {body!r}")
            # D-4 delivery channel: the machine scale rides the leg
            # command environment (a prefix variable), it is never baked
            # into the suite's own configuration.
            check("NLANG_GATE_MACHINE_SCALE=1.0" in body,
                  "machine scale missing from the leg command "
                  f"environment: {body!r}")
            # Leg output lands in a log file, so the leg environment pins
            # UTF-8 I/O (a low ANSI codepage machine would die on the
            # checker's Chinese advisory wording -- false red).
            check("PYTHONIOENCODING=utf-8" in body,
                  "leg environment lost the UTF-8 I/O pin: "
                  f"{body!r}")
            # Post-hoc budget judging needs the per-entry timings: the
            # leg writes its junit file, and the flat --timeout
            # watchdog now owns exactly the entries that carry no
            # derived TIMEOUT property (the row-less ones; Task 4 put
            # the per-row caps on every other entry at configure time).
            check("--output-junit" in body,
                  f"ctest leg writes no junit timings for the budget "
                  f"checker: {body!r}")
            # --output-on-failure (R-59): the junit carries timings and
            # verdicts but never the failing test's own output, so a
            # red ctest leg would otherwise force a manual re-run just
            # to see why.
            check("--output-on-failure" in body,
                  f"ctest leg lost --output-on-failure (R-59): "
                  f"{body!r}")
            check("--timeout 1500" in body,
                  f"ctest leg lost the --timeout fallback for row-less "
                  f"entries: {body!r}")
        # Each ctest leg gets its own <name>-budget gate.sh entry: a fused
        # word makes the leg's verdict unknowable (gate_selftest.sh
        # A-series lesson). It reuses the scalar calibration just measured
        # for this same leg (D-5); the stub-pinned 1.0 rides verbatim.
        budget_invocations = run_dir /"T1-debug-unit-budget.invocations"
        check(budget_invocations.exists(),
              "stub gate.sh never received T1-debug-unit-budget — the "
              "post-hoc checker leg never reached gate.sh")
        if budget_invocations.exists():
            body = budget_invocations.read_text()
            for token in ("check-budget", "--junit", "--config ctest-Debug",
                          "--scalar 1.0", "PYTHONIOENCODING=utf-8"):
                check(token in body,
                      f"budget checker invocation lost {token!r}: {body!r}")
        # The e2e legs print through the same file-bound channel, so the
        # UTF-8 pin must ride their environment too, and the judging
        # config must be spelled out: e2e-Debug's leg passes
        # --config=Debug so the runner never falls back to its tool-path
        # guess.
        e2e_invocations = run_dir /"T1-debug-e2e.invocations"
        check(e2e_invocations.exists(),
              "stub gate.sh never received T1-debug-e2e — dispatch broken")
        if e2e_invocations.exists():
            body = e2e_invocations.read_text()
            check("PYTHONIOENCODING=utf-8" in body,
                  "e2e leg environment lost the UTF-8 I/O pin: "
                  f"{body!r}")
            check("--config=Debug" in body,
                  f"e2e leg lost its judging --config: {body!r}")
            # T1 is the full-scan tier, so its e2e legs sweep the whole
            # manifest (D-7: slow exclusions are T0-round-only); the
            # explicit env pin also fixes the judging universe.
            check("NLANG_E2E_MANIFEST=" in body
                  and "tests/e2e/manifest.txt" in body,
                  "T1-debug-e2e must sweep the FULL manifest: "
                  f"{body!r}")
        # Slow-set exclusions (D-16/D-17): the baseline rows' seconds
        # against SLOW_MIN_REF_S are the single source for the slow
        # names, the perf differential (D-6) for the perf names -- the
        # one repo-wide roster projects onto every config's -E (the
        # ASan leg excludes names its tree never registered), and both
        # joins ride the leg command itself (F-12: -E beats any TIMEOUT
        # property).
        slow_roster_e = ("-E 'e2e_runner_timing_gate|mainwindow_tests|"
                         "mnist_mlp_compile|perf_stub_compile|"
                         "perf_stub_run'")
        for leg, slow_e in (
            ("T1-debug-unit", slow_roster_e),
            ("T1-ide-ctest", slow_roster_e),
            ("T1-asan-unit", slow_roster_e)):
            invocations = run_dir /f"{leg}.invocations"
            check(invocations.exists(),
                  f"stub gate.sh never received {leg} — dispatch broken")
            if invocations.exists():
                body = invocations.read_text()
                check(slow_e in body,
                      f"{leg} lost its slow-set exclusion {slow_e!r}: "
                      f"{body!r}")
        # R-30: every ctest leg gets a sibling check-timeouts entry as
        # its own gate.sh command, judging THAT leg's own build tree --
        # the --tree value is spelled out, never inferred (guard eleven).
        for leg, tree in (("T1-debug-unit", "master"),
                          ("T1-ide-ctest", "ide"),
                          ("T1-asan-unit", "asan")):
            invocations = run_dir /f"{leg}-timeouts.invocations"
            check(invocations.exists(),
                  f"stub gate.sh never received {leg}-timeouts (R-30)")
            if invocations.exists():
                body = invocations.read_text()
                check("check-timeouts" in body,
                      f"{leg}-timeouts lost the check-timeouts call: "
                      f"{body!r}")
                want = "--tree " + (sandbox / "trees" / tree).as_posix()
                check(want in body,
                      f"{leg}-timeouts must judge its own build tree "
                      f"({want!r} missing): {body!r}")
        # D-11 / --gate-env assembly: every sweep ctest leg declares its
        # own build tree and every e2e leg the repo-level tests/temp
        # scratch, as a leg-level --gate-env declaration that leg() turns
        # into an `env` prefix on the gate.sh call. The stub echoes the
        # variable as gate.sh's own process holds it, so a declaration
        # that only reached the inner command's environment (or never
        # left the recorded line) reads <unset>. The nide-gui claim rides
        # exactly the IDE-tree leg: the cache-shape predicate is the same
        # one guard eleven pairs trees with ctest-IDE on, and the roster
        # intersection runs on the leg's own selectors (mainwindow_tests
        # sits in the leg's -E, so only editor_tests can reach it).
        tree_abs = {name: (sandbox / "trees" / name).as_posix()
                    for name in ("master", "asan", "ide")}
        temp_abs = (sandbox / "tests" / "temp").as_posix()
        for leg, tree, claim in (("T1-debug-unit", "master", False),
                                 ("T1-ide-ctest", "ide", True),
                                 ("T1-asan-unit", "asan", False)):
            invocations = run_dir /f"{leg}.invocations"
            if not invocations.exists():
                continue  # the dispatch pins above already reported it
            body = invocations.read_text(encoding="utf-8")
            check("GATE_ENV NLANG_GATE_WRITE_DIR="
                  + tree_abs[tree] + "\n" in body,
                  f"{leg}: gate.sh's own environment lost the declared "
                  f"write dir: {body!r}")
            check(("--claim nide-gui" in body) == claim,
                  f"{leg}: the nide-gui claim declaration must be there "
                  f"exactly when the tree is IDE-built (expected "
                  f"{claim}): {body!r}")
        if e2e_invocations.exists():
            body = e2e_invocations.read_text(encoding="utf-8")
            check("GATE_ENV NLANG_GATE_WRITE_DIR=" + temp_abs + "\n"
                  in body,
                  "T1-debug-e2e: the runner's shared scratch was never "
                  f"declared to gate.sh: {body!r}")
            check("--claim" not in body,
                  "e2e legs hold no named resource today, so they must "
                  f"carry no claim: {body!r}")
        # The ASan split (the two differ exactly here): the leg sweeps
        # the e2e-ASan rows (--config=ASan) while its instrumented
        # binaries sit in /Debug/, so the judging key and the binary
        # path must not be conflated.
        asan_e2e = run_dir /"T1-asan-e2e-subset.invocations"
        check(asan_e2e.exists(), "stub gate never received asan-e2e-subset")
        if asan_e2e.exists():
            body = asan_e2e.read_text()
            check("--config=ASan" in body and "/Debug/" in body,
                  "the ASan e2e leg must judge from the e2e-ASan rows "
                  f"while running the Debug binaries: {body!r}")

        # Task 12 (R-91-C1): the coverage pair and the schedule core ride
        # every batch as their own gate.sh legs. The --enum NAME=FILE
        # pins name each config's universe after the trees THIS batch
        # was given (the checker's live walk would judge repo-relative
        # default trees instead), and --perf-names is load-bearing: the
        # coverage guards answer UNVERIFIED for every ctest-Release row
        # without the differential (R-61). The legs read only -- no
        # write directory, so gate.sh's own environment shows none (the
        # same no-directory face the budget and timeouts checker legs
        # show by design).
        tier_work = str(sandbox / "temp" / "tier")
        for leg_name, verb in (("T-guards-complete", "check-complete"),
                               ("T-guards-orphans", "check-orphans")):
            invocations = run_dir /f"{leg_name}.invocations"
            check(invocations.exists(),
                  f"stub gate.sh never received {leg_name} — the "
                  "coverage guard never reached gate.sh")
            if invocations.exists():
                body = invocations.read_text(encoding="utf-8")
                for token in (verb, "--table", "--enum ctest-Debug=",
                              "--enum ctest-IDE=", "--enum ctest-ASan=",
                              tier_work + "/guard_enum_ctest-Debug.txt",
                              "--perf-names",
                              tier_work + "/perf_names.txt"):
                    check(token in body,
                          f"{leg_name} lost {token!r} from the checker "
                          f"invocation: {body!r}")
                check("--gate-env" not in body,
                      f"{leg_name} only reads (show-only enumerations "
                      f"and table files), so no write-directory "
                      f"declaration may appear: {body!r}")
                check("GATE_ENV NLANG_GATE_WRITE_DIR=<unset>\n" in body,
                      f"{leg_name}: a read-only checker leg must show "
                      f"no write dir in gate.sh's environment: "
                      f"{body!r}")
        sched_invocations = run_dir /"T-schedule.invocations"
        check(sched_invocations.exists(),
              "stub gate.sh never received T-schedule — the schedule "
              "axis guard never reached gate.sh")
        if sched_invocations.exists():
            body = sched_invocations.read_text(encoding="utf-8")
            for token in ("check-schedule", "--config ctest-Debug",
                          "--perf-names", "--e2e-config e2e-Debug",
                          "--e2e-manifest"):
                check(token in body,
                      f"T-schedule lost {token!r}: {body!r}")
            check("--gate-env" not in body and "--mapping" not in body,
                  "the full-scan round has no fast round to backfill, "
                  "so its schedule guard runs with neither a write-dir "
                  f"declaration nor a --mapping: {body!r}")

        run_mapped_round(bash, sandbox)
        run_conservative_round(bash, sandbox)
        run_listing_rounds(bash, sandbox)
        return not failures
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)


def main():
    ok = run_smoke()
    verdict = "GATE gate_tier_smoke: " + ("PASS" if ok else "FAIL")
    print(verdict)
    for msg in failures:
        #Console codepage may not carry bash's raw bytes; keep it printable.
        safe = msg.encode("ascii", "replace").decode("ascii")
        print(f"  - {safe}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
