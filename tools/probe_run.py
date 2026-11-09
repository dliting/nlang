#!/usr/bin/env python
"""probe_run.py — compile and run a single NLang probe, reporting the
full 32-bit exit code.

Manual probes MUST go through this script instead of reading the exit
status through a POSIX shell: bash and cmd both expose only the low 8
bits ($? / %ERRORLEVEL%), so a probe returning 256 reads as 0 and one
returning 448 reads as 192 — which has more than once manufactured a
phantom VM bug out of a passing run. Two supported conventions:

  1. The probe asserts internally with a bounded sentinel and returns 0:
         if (acc != 131071) return 89;
         return 0;
     Verify with:  probe_run.py PROBE.n --expect 0
  2. The probe returns its accumulator directly; pass the true expected
     code:  probe_run.py PROBE.n --expect 131071  (no truncation here)

Usage:
  python tools/probe_run.py PROBE.n [--expect CODE] [--build DIR]
                                    [--keep] [-q]

--build defaults to <repo-root>/build; ncc/nvm are resolved under
<build>/src/tools/{ncc,nvm}/Release/. The compiled module is written
next to the probe as <stem>_probe.ncu (ncc rejects module names with
a second dot, so ".probe.ncu" is not an option; the name also never
clobbers e2e artifacts).
Compile diagnostics (stdout+stderr) print on failure; run stdout and
stderr print on separate labeled lines so merged-capture interleaving
can never forge output.
"""
import argparse
import subprocess
import sys
from pathlib import Path


def tool_path(build_root: Path, name: str) -> Path:
    return build_root / "src" / "tools" / name / "Release" / f"{name}.exe"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compile and run one NLang probe, full 32-bit exit code")
    ap.add_argument("probe", type=Path, help="the .n probe source")
    ap.add_argument("--expect", type=int, default=None,
                    help="expected 32-bit exit code (default: just report)")
    ap.add_argument("--build", type=Path, default=None,
                    help="build root (default: <repo-root>/build)")
    ap.add_argument("--keep", action="store_true",
                    help="print the module path instead of deleting it")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress run stdout/stderr echo")
    args = ap.parse_args()

    probe = args.probe.resolve()
    if not probe.is_file():
        print(f"probe not found: {probe}", file=sys.stderr)
        return 2
    build_root = (args.build or
                  Path(__file__).resolve().parents[1] / "build").resolve()
    ncc = tool_path(build_root, "ncc")
    nvm = tool_path(build_root, "nvm")
    for tool in (ncc, nvm):
        if not tool.is_file():
            print(f"tool not found: {tool}", file=sys.stderr)
            return 2

    module = probe.with_name(probe.stem + "_probe.ncu")
    #The "build" subcommand is mandatory: bare `ncc PROBE.n` compiles AND
    #executes, leaking the program's exit code as ncc's — exactly the
    #ambiguity this script exists to eliminate.
    cc = subprocess.run([str(ncc), "build", str(probe), "-o", str(module)],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        print(f"COMPILE FAILED (ncc exit {cc.returncode})")
        print(f"--- ncc stdout ---\n{cc.stdout}", end="")
        print(f"--- ncc stderr ---\n{cc.stderr}", end="")
        return 2
    if not module.is_file():
        print(f"ncc reported success but no module: {module}", file=sys.stderr)
        return 2

    run = subprocess.run([str(nvm), str(module)],
                         capture_output=True, text=True)
    if not args.quiet:
        if run.stdout:
            print(f"--- run stdout ---\n{run.stdout}", end="")
        if run.stderr:
            print(f"--- run stderr ---\n{run.stderr}", end="")
    if args.keep:
        print(f"module: {module}")
    else:
        module.unlink(missing_ok=True)

    print(f"exit code: {run.returncode}"
          + (f" (expected {args.expect})" if args.expect is not None else ""))
    if args.expect is not None and run.returncode != args.expect:
        print("FAIL")
        return 1
    if args.expect is not None:
        print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
