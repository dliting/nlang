# gate_tier_sandbox.py -- the sandbox fixture and shared verdict
# state behind the tier smoke (entry: gate_tier_test.py). Where to
# find the MSYS bash the tier script needs (find_git_bash: the bare
# PATH may hand back the WSL stub), how one stub batch launches
# (run_tier: pinned machine scale, stub cmake/ctest doors, pinned
# tier work directory), how each batch's run-scoped log directory is
# located after the fact (snapshot_runs / new_run_dir) and the single
# failures list every scenario
# module appends to (check). build_sandbox seeds the stub gate.sh,
# the stub cmake/ctest pair behind .bat doors, the seed tree caches,
# the tier/family/table/manifest files and copies of the gate-time
# tool modules, so the smoke runs the real gate_tier.sh assembly
# against nothing but stubs. Stdlib only; run with -I.

import os
import shutil
import subprocess
import sys
from pathlib import Path


failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def find_git_bash():
    """The tier script is MSYS bash on Windows, where the bare PATH may
    hand back the WSL stub (C:/Windows/System32/bash.exe) that cannot
    see Windows temp paths -- hence the MSYSTEM marker probe. A POSIX
    host has no stub hazard: its own /bin/bash runs the tier directly."""
    if os.name != "nt":
        return os.environ.get("NLANG_GIT_BASH") or shutil.which("bash")
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


def snapshot_runs(sandbox):
    """The run-scoped batch directories that exist under the sandbox's
    gates root at call time (empty before the first batch). Pair with
    new_run_dir: snapshot, run the batch, and the difference is exactly
    the directory that batch wrote -- batches never share a log
    directory, so a round reads only its own artifacts."""
    gates = sandbox / "temp" / "tier" / "gates"
    if not gates.exists():
        return set()
    return {d for d in gates.iterdir() if d.is_dir()}


def new_run_dir(sandbox, before):
    """The run directory one batch produced: exactly the snapshot
    difference. A batch that produced none (it died before its mkdir)
    yields None, which callers must check() before reading artifacts
    from it."""
    fresh = snapshot_runs(sandbox) - before
    return fresh.pop() if len(fresh) == 1 else None


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
    "gate_time_backfill.py",
    "gate_time_budget.py",
    "gate_time_carrier.py",
    "gate_time_check.py",
    "gate_time_cli.py",
    "gate_time_guards.py",
    "gate_time_measure.py",
    "gate_time_schedule.py",
    "gate_time_table.py",
    "gate_time_timeouts.py",
    "test_impact.py",
]

EXPECTED_LEGS = [
    # The freshness preflight is leg one of every tier; the stub
    # gate.sh PASSes it without executing the checker (the abort
    # semantics live in gate_selftest a19), so this pins only the
    # wiring.
    "T-preflight-fresh",
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


def build_sandbox(sandbox):
    """Populate a fresh sandbox for one stub batch: tool copies,
    seed tree caches, the stub trio behind .bat doors and the
    roster/family/table/manifest files the tier script reads.
    Callers keep ownership of the directory (the smoke removes it
    in its finally clause)."""
    repo = Path(__file__).resolve().parent.parent
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
