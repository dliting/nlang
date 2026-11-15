#!/usr/bin/env bash
# gate_selftest.sh — behaviour tests for gate.sh's write-directory lock.
#
# Two modes:
#   (no arguments)            driver: run the cases below, print one line per
#                             case, then one verdict line; exit with the
#                             number of failed cases.
#   --stub <label> <events> <go> [ignored...]
#                             the payload of a gate leg: append
#                             "start <label>" to <events>, wait until <go>
#                             appears, remove that file, then append
#                             "finish <label>". Every argument after <go> is
#                             ignored on purpose — the driver puts the
#                             directory flags there so the script under test
#                             derives the lock from the same spelling the
#                             real flags are written in, and the stub has no
#                             business parsing them.
#
# The gate script under test is <repo>/tools/gate.sh. NLANG_GATE_SH overrides
# that path: the mutation checks point it at a modified copy under temp/,
# which is the only way to test a broken gate.sh without editing the
# repository copy (the repository copy must not be edited at all while some
# other gate batch is running).
#
# All driver artifacts live in one private workspace: the test trees, the
# event files, the GO files, each leg's <stem>.console, and a private
# NLANG_GATE_LOCK_DIR so a selftest never queues behind a real gate batch.
# Assertions compare event order and log lines only — no timing assumptions,
# so a busy machine cannot turn a correct implementation red (spec D-10).

set -u

# One global budget, spent once. Per-case timeouts would let the cases sum to
# many minutes and hit the ctest TIMEOUT for this entry before the sentinel
# could print who holds the lock — the one clue the sentinel exists to print.
SELFTEST_SENTINEL_S=120

REPO=$(cd "$(dirname "$0")/.." && pwd)
GATE_SH=${NLANG_GATE_SH:-$REPO/tools/gate.sh}
SELF="$REPO/tools/gate_selftest.sh"

# --- stub mode --------------------------------------------------------------

if [ "${1:-}" = "--stub" ]; then
    if [ "$#" -lt 4 ]; then
        echo "usage: $SELF --stub <label> <events-file> <go-file> [ignored...]" >&2
        exit 64
    fi
    label="$2"; events="$3"; go="$4"
    printf 'start %s\n' "$label" >> "$events"
    # GO means "this leg may finish now". Removing it before recording the
    # finish is what keeps a leftover GO from releasing the next round's leg.
    while [ ! -e "$go" ]; do
        sleep "${NLANG_GATE_POLL_S:-1}"
    done
    rm -f "$go"
    printf 'finish %s\n' "$label" >> "$events"
    exit 0
fi

# --- driver mode ------------------------------------------------------------

WS=$(mktemp -d)
cd "$WS" || exit 1
# pwd, not $WS as mktemp printed it: with TMPDIR set to a Windows spelling
# (measured: TMPDIR=F:/temp/AppData) mktemp prints that spelling back, while
# pwd always gives the POSIX form. The relative spellings in the cases and the
# MSYS path rules both want the POSIX form.
WS=$(pwd)
export NLANG_GATE_LOCK_DIR="$WS/lock"
export NLANG_GATE_POLL_S=1
# A stray exported value would make the "no write directory derived" case
# measure the caller's environment instead of the command line; the cases
# that want it set it per command (spec D-3 forbids exporting it).
unset NLANG_GATE_WRITE_DIR

sentinel_deadline=$(( $(date +%s) + SELFTEST_SENTINEL_S ))

deadline() { # seconds left in the global budget, never negative
    left=$(( sentinel_deadline - $(date +%s) ))
    [ "$left" -gt 0 ] && echo "$left" || echo 0
}

pending_gos=()   # GO files still to hand out, released on the way out

cleanup() {
    # A leg whose case failed is still waiting for its GO; hand it out, then
    # wait, so no stub process outlives the workspace it writes into.
    local go
    for go in ${pending_gos[@]+"${pending_gos[@]}"}; do
        [ -e "$go" ] || touch "$go"
    done
    wait
    rm -rf "$WS"
}
trap cleanup EXIT

failures=0

pass() { printf 'selftest: PASS %s\n' "$1"; }

fail() { printf 'selftest: FAIL %s: %s\n' "$1" "$2"; failures=$(( failures + 1 )); }

skip() { printf 'selftest: SKIP %s: %s\n' "$1" "$2"; }

# await <file> <fixed-string>: poll until the string shows up in the file,
# using the remaining global budget as the only deadline.
await() {
    local file="$1" want="$2"
    while [ "$(deadline)" -gt 0 ]; do
        if [ -f "$file" ] && grep -F -q -e "$want" -- "$file"; then
            return 0
        fi
        sleep "$NLANG_GATE_POLL_S"
    done
    return 1
}

# count_lines <file> <fixed-string>: how many lines contain the string
# (0 when the file was never written).
count_lines() {
    if [ -f "$1" ]; then
        grep -F -c -e "$2" -- "$1"
    else
        echo 0
    fi
}

# go_file <case> <label>: the one-shot release signal for one leg of one case.
# Keyed by case name as well as label so a GO left over from an earlier case
# cannot release a later leg.
go_file() { printf '%s/%s-%s.go\n' "$WS" "$1" "$2"; }

# leg <name> <stem> <go-or-empty> <command...>: start one gate leg in the
# background. gate.sh writes the command's own output to <stem>.out/.err; the
# leg console holds what gate.sh itself prints (its LOCK and GATE lines), all
# of which it writes to stdout, so redirecting both streams here loses nothing.
leg() {
    local name="$1" stem="$2" go="$3"
    shift 3
    if [ -n "$go" ]; then
        pending_gos+=("$go")
    fi
    bash "$GATE_SH" --shared "$name" "$stem" "$@" > "$stem.console" 2>&1 &
}

# release_leg <go>: let one waiting leg run to its finish.
release_leg() {
    local go="$1"
    if [ -e "$go" ]; then
        return 0
    fi
    touch "$go"
}

# --- cases ------------------------------------------------------------------
# Run order is fixed (spec 八.3): the locking cases first, then the two that
# take no directory lock, so that a broken flag derivation shows up as a
# failure of the case that carries the mechanism rather than always as the
# print case. Later tasks insert their cases above these two.

case_a6_no_write_dir() {
    local name=a6 stem=a6
    # No directory flag at all, and the env channel is unset above: the only
    # way this leg can pass is by reporting that no lock was taken because
    # nothing was derived.
    leg "$name" "$stem" "" bash -c "exit 0"
    if ! await "$stem.console" "GATE $name: PASS"; then
        fail "$case_name" "leg never printed its verdict: $(cat "$stem.console" 2>/dev/null)"
        return
    fi
    local warned
    warned=$(count_lines "$stem.console" "no write directory derived")
    if [ "$warned" -ne 1 ]; then
        fail "$case_name" "expected exactly one 'no write directory derived' line, found $warned"
    else
        pass "$case_name"
    fi
}

# The print mode must fold every spelling of one directory onto one lock key;
# the eleven spellings below are listed in the plan because each one is a
# separate arm of the flag set (four long flags, one short flag, the '=' form,
# the relative form, the backslash form, and the env channel).
case_a7_print_write_dirs() {
    local dir="$WS/t1"
    mkdir -p "$dir"
    local abs="$dir"        # mktemp/pwd already gave the POSIX absolute form
    local rel="t1"          # the leg CWD is this workspace
    local bs
    # The Windows spelling a person pastes from Explorer. tr is the pinned
    # derivation: sed and ${x//...} double the backslashes here. cygpath -m
    # does not absolutize, so $dir has to be absolute already.
    bs=$(cygpath -m "$dir" | tr '/' '\\')

    local -a spellings=() lines=() rcs=()
    local tag rc line
    # a7_run <tag> <args...>: one print-mode run, captured separately.
    a7_run() {
        tag="$1"; shift
        bash "$GATE_SH" --print-write-dirs "$@" > "a7-$tag.out" 2> "a7-$tag.err"
        rc=$?
        line=$(grep -E -m 1 '^key [0-9a-f]{16} path ' -- "a7-$tag.out" 2>/dev/null)
        spellings+=("$tag"); rcs+=("$rc"); lines+=("$line")
    }
    a7_run build-abs --build "$abs"
    a7_run build-rel --build "$rel"
    a7_run build-eq "--build=$abs"
    a7_run bglue "-B$abs"
    a7_run bspace -B "$abs"
    a7_run build-bs --build "$bs"
    a7_run test-dir --test-dir "$abs"
    a7_run test-dir-eq "--test-dir=$abs"
    a7_run site-dir --site-dir "$abs"
    a7_run workdir --workdir "$abs"
    local env_line env_rc
    env_line=$(NLANG_GATE_WRITE_DIR="$abs" bash "$GATE_SH" --print-write-dirs 2> "a7-env.err")
    env_rc=$?
    spellings+=("env"); rcs+=("$env_rc"); lines+=("$env_line")

    local i bad="" seen=""
    for i in "${!spellings[@]}"; do
        tag=${spellings[$i]}; rc=${rcs[$i]}; line=${lines[$i]}
        if [ "$rc" -ne 0 ]; then
            bad="$bad $tag(rc=$rc)"
            continue
        fi
        if [ -z "$line" ]; then
            bad="$bad $tag(no-key-line)"
            continue
        fi
        # The path half has to be absolute: a spelling that does not get
        # absolutized would lock a different directory while still printing a
        # key, which is exactly how a silent parallel build starts.
        case ${line#* path } in
            /*|[A-Za-z]:/*) ;;
            *) bad="$bad $tag(relative-path)"; continue ;;
        esac
        if [ -z "$seen" ]; then
            seen="$line"
        elif [ "$line" != "$seen" ]; then
            bad="$bad $tag(different-line)"
        fi
    done
    if [ -n "$bad" ]; then
        fail "$case_name" "spellings that did not converge:$bad (first line: ${seen:-none})"
    else
        pass "$case_name"
    fi
}

run_case() { # run_case <name> <function>: no case runs once the budget is gone
    local case_name="$1"   # visible to the case body through bash's dynamic scope
    if [ "$(deadline)" -eq 0 ]; then
        fail "$1" "sentinel deadline already passed"
        return
    fi
    "$2"
}

# --- driver -----------------------------------------------------------------

run_case a6-no-write-dir case_a6_no_write_dir
run_case a7-print-write-dirs case_a7_print_write_dirs

verdict() {
    if [ "$failures" -eq 0 ]; then
        echo "SELFTEST: PASS"
    else
        echo "SELFTEST: FAIL n=$failures"
    fi
    exit "$failures"
}

verdict
