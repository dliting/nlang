#!/usr/bin/env bash
# gate.sh — run one gate command and report its REAL exit code.
#
# Why this exists: `ctest ... | tail` reports the pipe's exit code
# (tail's, always 0), which once turned a 4-failure ctest run into a
# recorded "all green". Every gate (ctest, e2e, docs builds) must run
# through this script instead of a piped tail.
#
# Usage: tools/gate.sh <name> <logfile-stem> <command> [args...]
#   - stdout and stderr are captured SEPARATELY to <stem>.out/.err
#     (merged capture can interleave and drop lines).
#   - The command runs without a pipe; its own exit code is final.
#   - Prints both tails, then one verdict line: "GATE <name>: PASS" or
#     "GATE <name>: FAIL rc=<n>". The script exits with that rc.
# Read a gate result ONLY from the verdict line or the exit code —
# never from a tail alone.

set -u

if [ "$#" -lt 3 ]; then
    echo "usage: tools/gate.sh <name> <logfile-stem> <command> [args...]" >&2
    exit 64
fi

name="$1"; stem="$2"; shift 2

"$@" >"$stem.out" 2>"$stem.err"
rc=$?

echo "----- $name: stdout tail ($stem.out) -----"
tail -n 15 "$stem.out"
echo "----- $name: stderr tail ($stem.err) -----"
tail -n 15 "$stem.err"

if [ "$rc" -eq 0 ]; then
    echo "GATE $name: PASS"
else
    echo "GATE $name: FAIL rc=$rc"
fi
exit "$rc"
