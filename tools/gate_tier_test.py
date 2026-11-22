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

        proc = subprocess.run(
            [bash,
             (sandbox / "tools" / "gate_tier.sh").as_posix(), "T1",
             "--master-tree", (sandbox / "trees" / "master").as_posix(),
             "--asan-tree", (sandbox / "trees" / "asan").as_posix(),
             "--ide-tree", (sandbox / "trees" / "ide").as_posix(),
             "--docs"],
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
        gates_dir = sandbox / "temp" / "tier" / "gates"
        runs = sorted(d for d in gates_dir.iterdir() if d.is_dir())
        check(len(runs) == 1,
              f"expected exactly one run-scoped log directory, found "
              f"{[d.name for d in runs]} (flat artifacts must not appear)")
        #Windows pathlib globs are case-insensitive, so the planted
        #t1_debug_unit.gate would match a T1-* glob; enumerate instead
        #and demand the flat root holds nothing but the stale plants.
        flat_files = {p.name for p in gates_root.iterdir() if p.is_file()}
        check(flat_files == {stale_same_name.name, stale_other_case.name},
              f"leg artifacts landed flat in the gates root ({sorted(
                  flat_files - {stale_same_name.name,
                                stale_other_case.name})}) — verdict "
              "files must live inside the per-run directory")
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
