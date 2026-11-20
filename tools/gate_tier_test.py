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
    "T1-debug-e2e",
    "T1-ide-ctest",
    "T1-asan-unit",
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
        (sandbox / "tools" / "gate.sh").write_text(STUB_GATE, newline="\n")
        (sandbox / "tools" / "gate_tiers.txt").write_text(TIERS, newline="\n")
        (sandbox / "tests" / "e2e" / "manifest.txt").write_text(MANIFEST, newline="\n")

        proc = subprocess.run(
            [bash,
             (sandbox / "tools" / "gate_tier.sh").as_posix(), "T1",
             "--master-tree", (sandbox / "trees" / "master").as_posix(),
             "--asan-tree", (sandbox / "trees" / "asan").as_posix(),
             "--ide-tree", (sandbox / "trees" / "ide").as_posix()],
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
