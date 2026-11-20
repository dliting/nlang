# e2e_shard.py -- manifest sharder for the tiered gates
# (docs/dev/gate_tiering_design.md). Splits an e2e manifest into K
# shards (deterministic round-robin, row text verbatim), runs the
# shards in parallel through run_e2e_tests.py with NLANG_E2E_MANIFEST,
# and aggregates one verdict. The A/B mode compares a serial full-suite
# run against a sharded run by per-entry verdict SETS — per the
# deterministic-test ruling the assertions are time-independent, so
# equal sets prove verdict equivalence; shards become a T1 default only
# after one such A/B passes on a fixed commit.
#
# Usage:
#   python e2e_shard.py run --ncc NCC --nvm NVM [--ndb NDB] --shards K
#         --work-dir DIR [--timeout SEC]
#   python e2e_shard.py ab --ncc NCC --nvm NVM --shards K --work-dir DIR
#         [--timeout SEC]
# Verdict line: "GATE e2e_shard_<mode>: PASS" (or FAIL), exit code
# follows.

import argparse
import os
import subprocess
import sys
from pathlib import Path

RUNNER = Path(__file__).resolve().parent.parent / "tests" / "e2e" / \
    "run_e2e_tests.py"


def parse_manifest(text):
    """Data rows only: comments and blank lines dropped, row text kept
    verbatim (the expectations column carries spaces)."""
    rows = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        rows.append(line.rstrip("\r\n").rstrip())
    return rows


def shard_rows(rows, shard_count):
    """Deterministic round-robin: row i goes to shard i % K, preserving
    manifest order within each shard. K above the row count clamps to
    one row per shard (empty shards would spawn pointless subprocess
    runs)."""
    shard_count = max(1, min(shard_count, len(rows)))
    shards = [[] for _ in range(shard_count)]
    for index, row in enumerate(rows):
        shards[index % shard_count].append(row)
    return shards


def write_shards(rows, shard_count, work_dir):
    """Write shard manifests next to the real one and return their
    paths. The override loader skips comments, so shards carry data
    rows only."""
    work_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for number, shard in enumerate(shard_rows(rows, shard_count), start=1):
        path = work_dir / ("shard_%03d.txt" % number)
        path.write_text("\n".join(shard) + "\n", encoding="utf-8",
                        newline="\n")
        paths.append(path)
    return paths


def parse_runner_output(text):
    """Per-entry verdict sets from a run_e2e_tests.py log: PASS lines
    and the trailing Failures: list ("<name>: <reason>")."""
    passed, failed = set(), set()
    in_failures = False
    for line in text.splitlines():
        if line.startswith("PASS "):
            passed.add(line.split()[1])
            continue
        if line.strip() == "Failures:":
            in_failures = True
            continue
        if in_failures and line.startswith("  "):
            failed.add(line.strip().split(":", 1)[0])
    return passed, failed


def ab_compare(serial_passed, serial_failed, shard_passed, shard_failed):
    """Verdict-set equality. Returns (match, human-readable diffs)."""
    diffs = []
    for name in sorted(serial_passed - shard_passed):
        diffs.append("serial PASS, shard not: %s" % name)
    for name in sorted(shard_passed - serial_passed):
        diffs.append("shard PASS, serial not: %s" % name)
    for name in sorted(serial_failed - shard_failed):
        diffs.append("serial FAIL, shard not: %s" % name)
    for name in sorted(shard_failed - serial_failed):
        diffs.append("shard FAIL, serial not: %s" % name)
    return (not diffs), diffs


def _run_one(runner_args, manifest_path, log_path, timeout):
    env = dict(os.environ)
    env["NLANG_E2E_MANIFEST"] = str(manifest_path)
    argv = [sys.executable, str(RUNNER)] + runner_args
    if timeout:
        argv.append("--timeout=%d" % timeout)
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.run(argv, env=env, stdout=log,
                              stderr=subprocess.STDOUT)
    return proc.returncode


def _collect(log_path):
    text = log_path.read_text(encoding="utf-8", errors="replace")
    return parse_runner_output(text)


def cmd_run(args):
    rows = parse_manifest(Path(args.manifest).read_text(encoding="utf-8"))
    work_dir = Path(args.work_dir)
    paths = write_shards(rows, args.shards, work_dir)
    tool_args = [args.ncc, args.nvm]
    if args.ndb:
        tool_args.append(args.ndb)
    results = []
    for path in paths:
        log = work_dir / (path.stem + ".log")
        rc = _run_one(tool_args, path, log, args.timeout)
        results.append((path.stem, rc))
    bad = [(name, rc) for name, rc in results if rc != 0]
    for name, rc in results:
        print("shard %s rc=%d" % (name, rc))
    if bad:
        print("GATE e2e_shard_run: FAIL (%d shard(s) red)" % len(bad))
        return 1
    print("GATE e2e_shard_run: PASS (%d shards)" % len(results))
    return 0


def cmd_ab(args):
    """Serial full-suite run vs sharded run on the same commit and
    tools: verdict SETS must be equal (deterministic assertions)."""
    work_dir = Path(args.work_dir)
    rows = parse_manifest(Path(args.manifest).read_text(encoding="utf-8"))
    tool_args = [args.ncc, args.nvm]
    if args.ndb:
        tool_args.append(args.ndb)
    serial_log = work_dir / "ab_serial.log"
    serial_rc = _run_one(tool_args, args.manifest, serial_log, args.timeout)
    paths = write_shards(rows, args.shards, work_dir)
    shard_rcs = []
    for path in paths:
        log = work_dir / (path.stem + ".log")
        shard_rcs.append(_run_one(tool_args, path, log, args.timeout))
    sp, sf = _collect(serial_log)
    shard_p, shard_f = set(), set()
    for path in paths:
        p, f = _collect(work_dir / (path.stem + ".log"))
        shard_p |= p
        shard_f |= f
    match, diffs = ab_compare(sp, sf, shard_p, shard_f)
    for d in diffs:
        print("DIFF: %s" % d)
    if serial_rc != 0 or any(rc != 0 for rc in shard_rcs):
        #A red underlying run makes the comparison void — the A/B
        #is only evidence on green runs.
        print("GATE e2e_shard_ab: FAIL (underlying run red: serial rc=%d)"
              % serial_rc)
        return 1
    if not match:
        print("GATE e2e_shard_ab: FAIL (%d verdict diffs)" % len(diffs))
        return 1
    print("GATE e2e_shard_ab: PASS (%d entries, %d shards)"
          % (len(sp) + len(sf), len(paths)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)

    for name in ("run", "ab"):
        p = sub.add_parser(name)
        p.add_argument("--manifest", required=True,
                       help="e2e manifest to shard")
        p.add_argument("--ncc", required=True)
        p.add_argument("--nvm", required=True)
        p.add_argument("--ndb")
        p.add_argument("--shards", type=int, required=True)
        p.add_argument("--work-dir", required=True)
        p.add_argument("--timeout", type=int, default=None)

    args = parser.parse_args()
    if args.mode == "run":
        sys.exit(cmd_run(args))
    sys.exit(cmd_ab(args))


if __name__ == "__main__":
    main()
