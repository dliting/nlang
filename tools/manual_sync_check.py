"""manual_sync_check.py - drift report between the user manual and the code.

The manual's coverage has an anchor: docs/user_manual/SYNC_BASELINE
holds the commit hash whose user-visible changes the manual already
reflects. This tool answers "what happened since": it lists every
commit between the baseline and HEAD, so a manual edit can account for
each one before shipping (the discipline is in AGENTS.md, "新增或变
更功能").

Checks, in order:
  1. SYNC_BASELINE exists and its first line starts with a hash that
     resolves to a commit;
  2. `git log <baseline>..HEAD` is empty (no drift) or is printed in
     full (drift - every line needs a deliberate "already reflected /
     not user-visible / reflected in this edit" decision).

Exit codes: 0 = no drift, 1 = drift (or a broken baseline), 2 = usage
or repository error. A release gate runs this tool and requires 0.

Usage:
    python tools/manual_sync_check.py            # report drift
    python tools/manual_sync_check.py --update   # set baseline to HEAD
                                               # (run right after a
                                               # manual-sync commit)
"""
import argparse
import subprocess
import sys
from pathlib import Path

BASELINE_REL = Path("docs/user_manual/SYNC_BASELINE")

EXIT_CLEAN = 0
EXIT_DRIFT = 1
EXIT_ERROR = 2


def run_git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    """One git invocation inside the repo; never raises."""
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def read_baseline(repo: Path) -> str:
    """First token of the baseline file: the commit hash."""
    path = repo / BASELINE_REL
    if not path.is_file():
        print(f"manual_sync_check: FAIL: {BASELINE_REL} is missing "
              "(create it: one commit hash per line, see the file's "
              "own header)")
        raise SystemExit(EXIT_ERROR)
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            return stripped.split()[0]
    print(f"manual_sync_check: FAIL: {BASELINE_REL} carries no hash "
          "on its first non-comment line")
    raise SystemExit(EXIT_ERROR)


def resolve(repo: Path, rev: str) -> str:
    """Full hash of rev, or FAIL (a rewritten-away anchor is drift)."""
    probe = run_git(repo, "rev-parse", "--verify", "--quiet",
                    f"{rev}^{{commit}}")
    if probe.returncode != 0 or not probe.stdout.strip():
        print(f"manual_sync_check: FAIL: baseline hash '{rev}' does "
              "not resolve to a commit (history rewritten? re-set it "
              "with --update after a full manual audit)")
        raise SystemExit(EXIT_ERROR)
    return probe.stdout.strip()


def report(repo: Path) -> int:
    baseline = resolve(repo, read_baseline(repo))
    log = run_git(repo, "log", "--oneline", f"{baseline}..HEAD")
    if log.returncode != 0:
        print("manual_sync_check: FAIL: git log failed:\n"
              + log.stderr.strip())
        raise SystemExit(EXIT_ERROR)
    entries = [line for line in log.stdout.splitlines() if line.strip()]
    if not entries:
        print(f"manual_sync_check: no drift - the manual covers "
              f"through {baseline[:12]}")
        return EXIT_CLEAN
    print(f"manual_sync_check: {len(entries)} commit(s) since the "
          f"baseline {baseline[:12]} - each needs a manual decision "
          "(reflected / not user-visible):")
    for line in entries:
        print(f"  {line}")
    return EXIT_DRIFT


def update(repo: Path) -> int:
    head = resolve(repo, "HEAD")
    path = repo / BASELINE_REL
    header = [
        "# Manual sync baseline - the commit whose user-visible",
        "# changes the manual already reflects. Update with",
        "# `python tools/manual_sync_check.py --update` as part of",
        "# every manual-sync commit (see AGENTS.md).",
        "",
    ]
    path.write_text("\n".join(header) + head[:12] + "\n",
                    encoding="utf-8", newline="\n")
    print(f"manual_sync_check: baseline set to {head[:12]}")
    return EXIT_CLEAN


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report manual/code drift against SYNC_BASELINE.")
    parser.add_argument(
        "--repo", default=str(Path(__file__).resolve().parent.parent),
        help="repository root (default: the checkout this file lives in)")
    parser.add_argument(
        "--update", action="store_true",
        help="write HEAD into SYNC_BASELINE (after a manual-sync commit)")
    args = parser.parse_args()
    repo = Path(args.repo)
    if not (repo / ".git").exists():
        print("manual_sync_check: FAIL: not a git repository: "
              f"{repo}")
        return EXIT_ERROR
    return update(repo) if args.update else report(repo)


if __name__ == "__main__":
    sys.exit(main())
