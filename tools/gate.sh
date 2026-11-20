#!/usr/bin/env bash
# gate.sh — run one gate command and report its REAL exit code.
#
# Why this exists: `ctest ... | tail` reports the pipe's exit code
# (tail's, always 0), which once turned a 4-failure ctest run into a
# recorded "all green". Every gate (ctest, e2e, docs builds) must run
# through this script instead of a piped tail.
#
# Usage: tools/gate.sh [--shared] <name> <logfile-stem> <command> [args...]
#   - stdout and stderr are captured SEPARATELY to <stem>.out/.err
#     (merged capture can interleave and drop lines).
#   - The command runs without a pipe; its own exit code is final.
#   - Prints both tails, then one verdict line: "GATE <name>: PASS" or
#     "GATE <name>: FAIL rc=<n>". The script exits with that rc.
# Read a gate result ONLY from the verdict line or the exit code —
# never from a tail alone. Wait/report lines use a "LOCK " prefix, so
# they can never be mistaken for a verdict.
#
# Machine lock: a test suite measures a machine it must own. Two worktree
# gate batches on one machine share the CPU (timing-sensitive VM and
# concurrency tests), the desktop (only the foreground process has a
# keyboard focus chain) and — until the tests got their own scratch trees
# — %TEMP%. So a gate that RUNS tests takes a machine-wide lock, and a
# gate that only BUILDS (--shared: cmake, vcpkg-style compiles, docs)
# waits for the test gates but still overlaps other build gates, which is
# what the parallel three-tree rebuild needs.
#
#   --shared  the command writes only inside its own build tree and is
#             safe next to another --shared gate; never next to a test gate
#             (leave it off for ctest, e2e, and anything timing- or
#             GUI-sensitive: forgetting the flag costs a wait, forgetting
#             it where it was needed costs a polluted measurement)
#
# The lock lives in $TMPDIR/nlang-gate-<user>/ by default (one directory
# per machine user, shared by every worktree); set NLANG_GATE_LOCK_DIR to
# point it somewhere else, which a CI runner with a per-job TMPDIR has to
# do to keep one machine's gates in one lock. The acquire and wait lines
# print the path, so a batch that took the wrong lock is visible in the
# log rather than silent. Holders are an atomic mkdir of exclusive/ for
# exclusive gates and one file each in shared/ for build gates. A holder
# has been held for more than LOCK_STALE_S is assumed dead — a killed or
# crashed gate — and is taken over, naming the holder it replaced. A long
# stream of --shared gates can in principle keep a waiting test gate
# waiting (the waiting gate prints its elapsed wait, so it is visible);
# in a batch the test gates come last anyway.
#
# Env overrides, for the probes in temp/ and for CI:
#   NLANG_GATE_LOCK_DIR (the lock directory itself), NLANG_GATE_POLL_S,
#   NLANG_GATE_REPORT_EVERY_S, NLANG_GATE_STALE_S.

set -u

LOCK_POLL_S=${NLANG_GATE_POLL_S:-5}
LOCK_REPORT_EVERY_S=${NLANG_GATE_REPORT_EVERY_S:-60}
LOCK_STALE_S=${NLANG_GATE_STALE_S:-10800}

# USER is set but empty in some MSYS shells, so fall through on empty
# values rather than locking under an anonymous name.
lock_owner="${USER:-${USERNAME:-unknown}}"
lock_dir="${NLANG_GATE_LOCK_DIR:-${TMPDIR:-/tmp}/nlang-gate-$lock_owner}"
excl_dir="$lock_dir/exclusive"
shared_dir="$lock_dir/shared"

usage() {
    echo "usage: tools/gate.sh [--shared] <name> <logfile-stem> <command> [args...]" >&2
    exit 64
}

shared_mode=0
if [ "${1:-}" = "--shared" ]; then
    shared_mode=1
    shift
fi
if [ "$#" -lt 3 ]; then
    usage
fi

name="$1"; stem="$2"; shift 2
gate_cmd="$*"

# One line per holder: who, since when, where, doing what. Read back
# verbatim when someone has to wait or when a stale lock is taken over.
write_holder() {
    {
        printf 'name=%s\n' "$name"
        printf 'pid=%s\n' "$$"
        printf 'started=%s\n' "$(date +%s)"
        printf 'dir=%s\n' "$PWD"
        printf 'mode=%s\n' "$([ "$shared_mode" -eq 1 ] && echo shared || echo exclusive)"
        printf 'cmd=%s\n' "$gate_cmd"
    } > "$1"
}

# Contents of a holder file, flattened onto one line for a wait message.
holder_text() {
    if [ -f "$1" ]; then
        tr '\n' ' ' < "$1"
    else
        printf 'unknown holder'
    fi
}

# Seconds a holder file has been open, or 0 when it says nothing useful.
holder_age_s() {
    started=$(sed -n 's/^started=\([0-9][0-9]*\)$/\1/p' "$1" 2>/dev/null)
    if [ -n "${started:-}" ]; then
        echo $(( $(date +%s) - started ))
    else
        echo 0
    fi
}

# Drop holders that outlived their process (a gate killed with the shell
# closed leaves its directory behind, and that must not freeze the
# machine until someone notices).
sweep_stale() {
    if [ -d "$excl_dir" ]; then
        age=$(holder_age_s "$excl_dir/holder.info")
        if [ "$age" -gt "$LOCK_STALE_S" ]; then
            echo "LOCK $name: taking over an exclusive lock held ${age}s by $(holder_text "$excl_dir/holder.info")"
            rm -rf "$excl_dir"
        fi
    fi
    for file in "$shared_dir"/*.info; do
        [ -f "$file" ] || continue
        age=$(holder_age_s "$file")
        if [ "$age" -gt "$LOCK_STALE_S" ]; then
            echo "LOCK $name: dropping a shared lock held ${age}s by $(holder_text "$file")"
            rm -f "$file"
        fi
    done
}

# Report the current holder at most once a minute while waiting; the
# first line also names the lock directory, which is what shows up when
# two batches silently took two different locks.
wait_round() { # <waited-seconds> <holder-description>
    if [ "$1" -eq 0 ]; then
        echo "LOCK $name: waiting for the machine lock in $lock_dir ($2)"
    elif [ $(( $1 % LOCK_REPORT_EVERY_S )) -eq 0 ]; then
        echo "LOCK $name: still waiting ${1}s ($2)"
    fi
}

acquire() {
    waited=0
    while :; do
        # The root dir is never deleted (see release), but a temp cleaner
        # or a manual sweep can take it away, and then the atomic claim
        # below would fail forever.
        mkdir -p "$lock_dir"
        sweep_stale
        if [ "$shared_mode" -eq 1 ]; then
            # A build gate only has to stay out of a test gate's way.
            if [ ! -e "$excl_dir" ]; then
                mkdir -p "$shared_dir"
                write_holder "$shared_dir/$name-$$.info"
                echo "LOCK $name: shared lock acquired after ${waited}s ($lock_dir)"
                return 0
            fi
            desc="exclusive lock held $(holder_age_s "$excl_dir/holder.info")s by $(holder_text "$excl_dir/holder.info")"
        else
            # A test gate wants the machine to itself: no other exclusive
            # holder (mkdir is the atomic claim, so a losing racer just
            # loops) and no build gate compiling next to it.
            if [ ! -e "$excl_dir" ] && [ -z "$(ls -A "$shared_dir" 2>/dev/null)" ]; then
                if mkdir "$excl_dir" 2>/dev/null; then
                    write_holder "$excl_dir/holder.info"
                    echo "LOCK $name: machine lock acquired after ${waited}s ($lock_dir)"
                    return 0
                fi
                desc="exclusive lock claimed by another waiting gate"
            else
                desc="exclusive lock held $(holder_age_s "$excl_dir/holder.info")s by $(holder_text "$excl_dir/holder.info")"
            fi
        fi
        wait_round "$waited" "$desc"
        sleep "$LOCK_POLL_S"
        waited=$(( waited + LOCK_POLL_S ))
    done
}

release() {
    if [ "$shared_mode" -eq 1 ]; then
        rm -f "$shared_dir/$name-$$.info"
    else
        rm -rf "$excl_dir"
    fi
    # Only the holder files go away; the root stays, so the atomic claim
    # always has its parent to work in.
    rmdir "$shared_dir" 2>/dev/null || true
}

mkdir -p "$lock_dir"
acquire
trap release EXIT

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
