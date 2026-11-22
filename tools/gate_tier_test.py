# gate_tier_test.py -- end-to-end smoke for the leg dispatch path.
# Pins the launch wiring of gate_tier.sh: every leg must actually reach
# tools/gate.sh. A stub gate.sh stands in for the real one (records the
# command it received, prints a PASS verdict), so the test exercises the
# tier assembly in a sandbox without running any real suite. A quoting
# bug that fuses "bash <path>" into one command word shows up here as
# rc=127 legs and NO-VERDICT lines. Run:
# python gate_tier_test.py
# Verdict line: "GATE gate_tier_smoke: PASS" (or FAIL), exit code follows.

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

failures = []


def find_git_bash():
    """The tier script is MSYS bash; the bare PATH may hand back the
    WSL stub (C:/Windows/System32/bash.exe), which cannot see Windows
    temp paths. Probe candidates for the MSYSTEM marker Git Bash sets."""
    candidates = [
        os.environ.get("NLANG_GIT_BASH"),
        shutil.which("bash"),
        "D:/dev/Git/usr/bin/bash.exe",
        "D:/dev/Git/bin/bash.exe",
        "C:/Program Files/Git/usr/bin/bash.exe",
        "C:/Program Files/Git/bin/bash.exe",
    ]
    for exe in candidates:
        if not exe or not Path(exe).exists():
            continue
        try:
            probe = subprocess.run(
                [exe, "-c", "echo $MSYSTEM"],
                capture_output=True, encoding="utf-8", errors="replace",
                timeout=15)
        except (OSError, subprocess.SubprocessError):
            continue
        if "MINGW" in probe.stdout or "MSYS" in probe.stdout:
            return exe
    return None


def check(cond, msg):
    if not cond:
        failures.append(msg)


def run_tier(bash, sandbox, *args):
    """One tier batch invocation in the sandbox. The machine scale is
    pinned (D-4) so the stub trees need no calibration sampling, and
    the cmake/ctest doors (NLANG_GATE_CMAKE / NLANG_GATE_CTEST) point
    at the stub .bat pair so print-perf-names' scratch configures
    (D-6: the perf set is the ON/OFF registration differential) never
    touch the real toolchain."""
    return subprocess.run(
        [bash, (sandbox / "tools" / "gate_tier.sh").as_posix()]
        + list(args),
        env=dict(os.environ, NLANG_GATE_MACHINE_SCALE="1.0",
                 NLANG_GATE_TIER_WORK=str(sandbox / "temp" / "tier"),
                 NLANG_GATE_CMAKE=str(sandbox / "tools"
                                      / "stub-cmake.bat"),
                 NLANG_GATE_CTEST=str(sandbox / "tools"
                                      / "stub-ctest.bat")),
        capture_output=True, encoding="utf-8", errors="replace",
        timeout=120, cwd=str(sandbox))


TIERS = """\
claim nide-gui mainwindow_tests editor_tests
tier T1-debug-e2e selector .
tier T1-asan-subset selector smoke
tier T2-debug-e2e selector .
tier T2-asan-e2e selector .
tier T2-release-e2e selector .
"""

MANIFEST = """\
#header
smoke_one 42
stdlib_stub_size 5
"""

STUB_GATE = """\
#!/usr/bin/env bash
#Stub gate.sh: record what the tier assembly hands over, then pass.
#The leading --claim options precede the leg name (gate timing D-11;
#the machine lock's --shared retired with it, D-10), so skip them the
#way the real gate.sh parses them -- but record
#the full argument line as received, because the assembly pins check
#the claim declaration itself, and echo the gate-level environment the
#stub process holds: an --gate-env that only reached the inner command
#would show <unset> here instead of the declared path.
raw="$*"
while [ "$1" = "--claim" ]; do
  [ "$1" = "--claim" ] && shift
  shift
done
name="$1"; stem="$2"; shift 2
{
  printf '%s\\n' "$raw"
  printf 'GATE_ENV NLANG_GATE_WRITE_DIR=%s\\n' "${NLANG_GATE_WRITE_DIR-<unset>}"
} >> "$stem.invocations"
echo "GATE $name: PASS"
"""

# Stub cmake/ctest for print-perf-names (D-6): cmake records the flag it
# was handed, ctest enumerates the ON side as {perf_stub_compile,
# perf_stub_run} -- names the stub table below does NOT hold, so the -E
# union can only come from the registration differential.
STUB_CMAKE = '''\
import os, sys
a = sys.argv[1:]
b = a[a.index("-B") + 1]
os.makedirs(b, exist_ok=True)
flag = "OFF"
for t in a:
    if t.startswith("-DNLANG_PERF_TESTS="):
        flag = t.split("=", 1)[1]
with open(os.path.join(b, "perf.flag"), "w") as fh:
    fh.write(flag)
'''

STUB_CTEST = '''\
import json, os, sys
a = sys.argv[1:]
tree = a[a.index("--test-dir") + 1]
names = []
flag = os.path.join(tree, "perf.flag")
if os.path.isfile(flag):
    with open(flag) as fh:
        if fh.read().strip() == "ON":
            names = ["perf_stub_compile", "perf_stub_run"]
print(json.dumps({"tests": [{"name": n, "properties": []}
                            for n in names]}))
'''

# Stub reference table for the schedule wiring: the slow set is no
# column (D-16/D-17) -- membership comes from the entry's baseline row
# (ctest-Debug, then ctest-IDE, then e2e-Debug, first hit) exceeding
# SLOW_MIN_REF_S, so mainwindow_tests needs its ctest-IDE row to be a
# slow member at all: the ASan-only reading never classifies. The one
# repo-wide roster projects onto every config's -E, so all three ctest
# legs exclude the same names. The e2e-Debug rows feed the derived-
# subset check: the explicit non-default line outside the slow set must
# join the mapping hits in manifest order.
REF_TABLE = """\
# stub reference table
ctest-Debug default 10.0 1 ctest-junit-serial - stub
ctest-Debug e2e_runner_timing_gate 409.77 1 ctest-junit-serial - timing probes carried elsewhere every round
ctest-Debug mnist_mlp_compile 220.0 1 ctest-junit-serial - demo class carried by T2
ctest-IDE default 10.0 1 ctest-junit-serial - stub
ctest-IDE e2e_runner_timing_gate 409.80 1 ctest-junit-serial - timing probes carried elsewhere every round
ctest-IDE mnist_mlp_compile 240.0 1 ctest-junit-serial - demo class carried by T2
ctest-IDE mainwindow_tests 115.60 1 ctest-junit-serial - slow on its baseline row
ctest-ASan default 10.0 1 ctest-junit-serial - stub
ctest-ASan mainwindow_tests 372.19 1 ctest-junit-serial - instrumented face carried by T2
e2e-Debug default 10.0 1 e2e-runner-serial - stub
e2e-Debug stdlib_stub_size 30.0 1 e2e-runner-serial - stub reason
"""

# Stub family file: the tensor family as D-7 specifies it, so a changed
# src/native/tensor/ path maps unit "mnist_mlp_compile|mnist_mlp_run"
# and e2e "smoke_" -- the T0-unit -E must subtract the mapped slow name
# while T1's -E carries the full slow set.
FAMILIES = """\
family tensor src src/native/tensor/**
family tensor unit mnist_mlp_compile|mnist_mlp_run
family tensor e2e smoke_
"""

# gate_tier.sh assembles the schedule-axis exclusions by calling
# gate_time_check.py print-slow / derive-e2e-subset against the script's
# own repo, so the sandbox needs the checker module chain next to its
# copy of the tier script.
TOOLS_MODULES = [
    "gate_time_carrier.py",
    "gate_time_check.py",
    "gate_time_guards.py",
    "gate_time_measure.py",
    "gate_time_schedule.py",
    "gate_time_table.py",
    "gate_time_timeouts.py",
    "test_impact.py",
]

EXPECTED_LEGS = [
    "T1-debug-unit",
    "T1-debug-unit-budget",
    "T1-debug-unit-timeouts",
    "T1-debug-e2e",
    "T1-ide-ctest",
    "T1-ide-ctest-budget",
    "T1-ide-ctest-timeouts",
    "T1-asan-unit",
    "T1-asan-unit-budget",
    "T1-asan-unit-timeouts",
    "T1-asan-e2e-subset",
    # Task 12: the coverage pair and the schedule core ride every batch.
    "T-guards-complete",
    "T-guards-orphans",
    "T-schedule",
]


def run_smoke():
    bash = find_git_bash()
    check(bash is not None, "no Git Bash found (MSYSTEM probe failed "
          "for every candidate); set NLANG_GIT_BASH")
    if bash is None:
        return False
    repo = Path(__file__).resolve().parent.parent
    sandbox = Path(tempfile.mkdtemp(prefix="gate-tier-smoke-"))
    try:
        (sandbox / "tools").mkdir()
        (sandbox / "tests" / "e2e").mkdir(parents=True)
        for tree in ("master", "asan", "ide"):
            (sandbox / "trees" / tree).mkdir(parents=True)
        # Seed cache shapes (guard eleven's config pairing reads them):
        # plain, sanitizer-flagged, IDE-built; master doubles as
        # print-perf-names' --seed-tree (D-6).
        for key, cache in (("master", "CMAKE_GENERATOR:INTERNAL=StubVS\n"),
                           ("asan", "CMAKE_CXX_FLAGS:STRING="
                                    "-fsanitize=address\n"),
                           ("ide", "NLANG_BUILD_IDE:BOOL=ON\n")):
            (sandbox / "trees" / key / "CMakeCache.txt").write_text(
                cache, encoding="utf-8")
        shutil.copy(repo / "tools" / "gate_tier.sh", sandbox / "tools" / "gate_tier.sh")
        # write_text(newline=) is 3.10+ and the registered python is older,
        # so the LF pins go through open(): bash files must not gain CRLF.
        def write_lf(path, text):
            with path.open("w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
        write_lf(sandbox / "tools" / "gate.sh", STUB_GATE)
        write_lf(sandbox / "tools" / "stub-cmake.py", STUB_CMAKE)
        write_lf(sandbox / "tools" / "stub-ctest.py", STUB_CTEST)
        for label, script in (("cmake", "stub-cmake.py"),
                              ("ctest", "stub-ctest.py")):
            # .bat form because CreateProcess spawns no bash script --
            # the same door shape the timeouts corpus pins.
            (sandbox / "tools" / ("stub-%s.bat" % label)).write_text(
                '@echo off\r\n"%s" -I "%s" %%*\r\n'
                % (sys.executable, sandbox / "tools" / script),
                encoding="ascii")
        write_lf(sandbox / "tools" / "gate_tiers.txt", TIERS)
        write_lf(sandbox / "tools" / "test_families.txt", FAMILIES)
        write_lf(sandbox / "tests" / "e2e" / "manifest.txt", MANIFEST)
        write_lf(sandbox / "tests" / "gate_time_reference.txt",
                 REF_TABLE)
        for module in TOOLS_MODULES:
            #The tier script calls print-slow / derive-e2e-subset
            #against its own repo; a missing module shows up as a red
            #leg, not as a copy failure (schedule axis lands with it).
            src = repo / "tools" / module
            if src.exists():
                shutil.copy(src, sandbox / "tools" / module)

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
        proc = run_tier(bash, sandbox, "T1", *trees)
        output = proc.stdout + proc.stderr

        check(proc.returncode == 0,
              f"tier batch exited rc={proc.returncode}; output:\n{output}")
        check("rc=127" not in output, "rc=127 leg launch: command word fused")
        for leg in EXPECTED_LEGS:
            check(f"GATE {leg}: PASS" in output, f"no PASS verdict for {leg}")
        gates_dir = sandbox / "temp" / "tier" / "gates"
        unit_invocations = gates_dir / "T1-debug-unit.invocations"
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
        budget_invocations = gates_dir / "T1-debug-unit-budget.invocations"
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
        e2e_invocations = gates_dir / "T1-debug-e2e.invocations"
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
            invocations = gates_dir / f"{leg}.invocations"
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
            invocations = gates_dir / f"{leg}-timeouts.invocations"
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
            invocations = gates_dir / f"{leg}.invocations"
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
        asan_e2e = gates_dir / "T1-asan-e2e-subset.invocations"
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
            invocations = gates_dir / f"{leg_name}.invocations"
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
        sched_invocations = gates_dir / "T-schedule.invocations"
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

        # Tier reuse: T2's copies of the shared ctest legs must NOT carry
        # the slow-set -E (the close-out sweep covers those entries),
        # while T1's own assembly does (t1_legs' slow_e parameter). The
        # perf names join EVERY ctest leg's -E either way (D-6: a
        # scheduled round may only run under the OFF-cache registration).
        dry1 = run_tier(bash, sandbox, "T1", *trees, "--dry-run")
        dry2 = run_tier(bash, sandbox, "T2", *trees, "--dry-run")
        for name, proc_dry in (("T1", dry1), ("T2", dry2)):
            check(proc_dry.returncode == 0,
                  f"{name} --dry-run exited rc={proc_dry.returncode}; "
                  f"output:\n{proc_dry.stdout + proc_dry.stderr}")
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
