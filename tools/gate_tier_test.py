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


TIERS = """\
tier T1-debug-e2e selector .
tier T1-asan-subset selector smoke
tier T2-debug-e2e selector .
tier T2-asan-e2e selector .
tier T2-release-e2e selector .
"""

MANIFEST = """\
#header
smoke_one 42
"""

STUB_GATE = """\
#!/usr/bin/env bash
#Stub gate.sh: record what the tier assembly hands over, then pass.
name="$1"; stem="$2"; shift 2
printf '%s\\n' "$*" >> "$stem.invocations"
echo "GATE $name: PASS"
"""

EXPECTED_LEGS = [
    "T1-debug-unit",
    "T1-debug-unit-budget",
    "T1-debug-e2e",
    "T1-ide-ctest",
    "T1-ide-ctest-budget",
    "T1-asan-unit",
    "T1-asan-unit-budget",
    "T1-asan-e2e-subset",
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
        shutil.copy(repo / "tools" / "gate_tier.sh", sandbox / "tools" / "gate_tier.sh")
        # write_text(newline=) is 3.10+; the registered ${PYTHON3_EXECUTABLE}
        # (the 9.3 line) is older, so the LF pins go through open(). The bash
        # scripts must not gain CRLF translations on a Windows checkout.
        def write_lf(path, text):
            with path.open("w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
        write_lf(sandbox / "tools" / "gate.sh", STUB_GATE)
        write_lf(sandbox / "tools" / "gate_tiers.txt", TIERS)
        write_lf(sandbox / "tests" / "e2e" / "manifest.txt", MANIFEST)

        # NLANG_GATE_MACHINE_SCALE pins the per-leg calibration (D-4): the
        # stub trees hold no configured suite, so an unpinned assembly
        # would burn live sampling rounds against them and every timed leg
        # would fail on the calibration, not on the wiring under test.
        proc = subprocess.run(
            [bash,
             (sandbox / "tools" / "gate_tier.sh").as_posix(), "T1",
             "--master-tree", (sandbox / "trees" / "master").as_posix(),
             "--asan-tree", (sandbox / "trees" / "asan").as_posix(),
             "--ide-tree", (sandbox / "trees" / "ide").as_posix()],
            env=dict(os.environ, NLANG_GATE_MACHINE_SCALE="1.0"),
            capture_output=True, encoding="utf-8", errors="replace",
            timeout=120)
        output = proc.stdout + proc.stderr

        check(proc.returncode == 0,
              f"tier batch exited rc={proc.returncode}; output:\n{output}")
        check("rc=127" not in output,
              "rc=127 leg launch (command word fused; see batch output above)")
        for leg in EXPECTED_LEGS:
            check(f"GATE {leg}: PASS" in output,
                  f"no PASS verdict for {leg} (see batch output above)")
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
            # The leg's output lands in a log file, so the leg
            # environment pins UTF-8 I/O for its Python processes (a
            # low ANSI codepage machine would otherwise die on the
            # checker's Chinese advisory wording -- false red).
            check("PYTHONIOENCODING=utf-8" in body,
                  "leg environment lost the UTF-8 I/O pin: "
                  f"{body!r}")
            # Post-hoc budget judging needs the per-entry timings: the
            # leg writes its junit file and keeps the transitional
            # --timeout watchdog fallback until the TIMEOUT properties
            # replace it (the caps consumer, Task 4).
            check("--output-junit" in body,
                  f"ctest leg writes no junit timings for the budget "
                  f"checker: {body!r}")
            check("--timeout 1500" in body,
                  f"ctest leg lost the transitional --timeout fallback: "
                  f"{body!r}")
        # Each ctest leg gets its own <name>-budget gate.sh entry (the
        # checker must not be spliced into the leg's own command string —
        # gate_selftest.sh A-series lesson: a fused word makes the leg's
        # verdict unknowable). It reuses the scalar that calibration just
        # measured for this same leg (D-5), and the stub-pinned 1.0 must
        # ride through verbatim.
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
        # UTF-8 pin must ride their environment too.
        e2e_invocations = gates_dir / "T1-debug-e2e.invocations"
        check(e2e_invocations.exists(),
              "stub gate.sh never received T1-debug-e2e — dispatch broken")
        if e2e_invocations.exists():
            body = e2e_invocations.read_text()
            check("PYTHONIOENCODING=utf-8" in body,
                  "e2e leg environment lost the UTF-8 I/O pin: "
                  f"{body!r}")
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
