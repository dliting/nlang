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

# await_holder <count>: poll until exactly <count> write-directory holder
# files exist, then print the path= line of each. The driver reads the
# key-to-directory pairing out of the holder files rather than re-running the
# normalization recipe itself: a second copy of that recipe in the test would
# turn red twice over whenever the implementation changes, and a green run
# would then prove nothing about the pairing (spec 八.2). find instead of a
# glob, because an unmatched glob stays literal and prints its own
# no-such-file line into the result.
await_holder() {
    local want="$1" n files f
    while :; do
        files=$(find "$WS/lock/wdir" -mindepth 2 -maxdepth 2 -name holder.info -print 2>/dev/null)
        n=$(printf '%s\n' "$files" | grep -c .)
        [ "$n" -eq "$want" ] && break
        [ "$(deadline)" -gt 0 ] || return 1
        sleep "$NLANG_GATE_POLL_S"
    done
    while IFS= read -r f; do
        [ -n "$f" ] && sed -n 's/^path=//p' "$f"
    done <<< "$files"
}

# holder_dump: every write-directory holder as it stands right now, flattened
# onto one line each. Printed on a failure, because who holds the lock is the
# one clue a red run has to carry out of the workspace.
holder_dump() {
    local f
    while IFS= read -r f; do
        [ -n "$f" ] && printf '%s {%s}\n' "$f" "$(tr '\n' ' ' < "$f")"
    done < <(find "$WS/lock/wdir" -mindepth 2 -maxdepth 2 -name holder.info -print 2>/dev/null)
}

# one_tree_pair <stem-A> <stem-B> <events> <flag-A> <dir-A> <flag-B> <dir-B>:
# the two-leg protocol both "same tree, two spellings" cases share. Leg A
# takes the tree and holds it until its GO, so leg B has to wait — deterministically,
# because a stub only finishes on its GO, whatever the machine is doing. The
# failure reasons are printed here, through the case name run_case set, so
# the protocol and its evidence exist in exactly one copy.
one_tree_pair() {
    local stemA="$1" stemB="$2" ev="$3" flagA="$4" dirA="$5" flagB="$6" dirB="$7"
    local goA goB holder_path want first
    goA=$(go_file "$case_name" A)
    goB=$(go_file "$case_name" B)
    leg "$stemA" "$stemA" "$goA" bash "$SELF" --stub A "$ev" "$goA" "$flagA" "$dirA"
    if ! await "$ev" "start A"; then
        fail "$case_name" "leg A never started: events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    if ! holder_path=$(await_holder 1); then
        fail "$case_name" "no single write-directory holder appeared: holders=[$(holder_dump)] events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    leg "$stemB" "$stemB" "$goB" bash "$SELF" --stub B "$ev" "$goB" "$flagB" "$dirB"
    # The wait line has to name the directory the holder recorded, which is
    # what shows B waiting for A's tree and not for something unrelated; the
    # trailing "(" makes the directory half of the line exact. This is the
    # write-directory lock's own first-round wording, so it cannot be confused
    # with the machine lock's wait line or with the load-dependent repeats.
    want="LOCK $stemB: waiting for the write-directory lock on $holder_path ("
    if ! await "$stemB.console" "$want"; then
        fail "$case_name" "leg B never printed the write-directory wait line for $holder_path: console=[$(cat "$stemB.console" 2>/dev/null)] holders=[$(holder_dump)]"
        return
    fi
    release_leg "$goA"
    if ! await "$ev" "finish A" || ! await "$stemA.console" "GATE $stemA: PASS"; then
        fail "$case_name" "leg A never finished: console=[$(cat "$stemA.console" 2>/dev/null)]"
        return
    fi
    release_leg "$goB"
    if ! await "$ev" "finish B" || ! await "$stemB.console" "GATE $stemB: PASS"; then
        fail "$case_name" "leg B never finished: console=[$(cat "$stemB.console" 2>/dev/null)]"
        return
    fi
    # The event order, not any log line, is the proof that the two legs never
    # wrote the tree at the same time.
    local want_events="start A
finish A
start B
finish B"
    if [ "$(cat "$ev" 2>/dev/null)" != "$want_events" ]; then
        fail "$case_name" "legs not serialized: events=[$(tr '\n' ';' < "$ev" 2>/dev/null)]"
        return
    fi
    # Exactly one first-round wait line (the implementation prints it on the
    # waited==0 round). Whether there were also repeat lines depends on how
    # long B had to wait, which depends on machine load, so repeats are
    # deliberately not asserted (spec D-10).
    first=$(count_lines "$stemB.console" "$want")
    if [ "$first" -ne 1 ]; then
        fail "$case_name" "expected one write-directory wait line, found $first"
    else
        pass "$case_name"
    fi
}

# --- cases ------------------------------------------------------------------
# Run order is fixed (spec 八.3): the locking cases first, then the two that
# take no directory lock, so that a broken flag derivation shows up as a
# failure of the case that carries the mechanism rather than always as the
# print case. Later tasks insert their cases above these two.

case_a1_same_tree_spellings() {
    # The same tree named two ways: absolute, and relative to the leg's own
    # working directory (which is this workspace).
    local dir="$WS/t1"
    mkdir -p "$dir"
    one_tree_pair a1A a1B "$WS/a1.events" --build "$dir" --build t1
}

case_a2_same_tree_case() {
    local dir="$WS/t2"
    mkdir -p "$dir"
    if ! command -v cygpath > /dev/null 2>&1; then
        # A case-sensitive file system really does have two different
        # directories here, so there is nothing for the fold to reconcile.
        skip "$case_name" "no cygpath: this file system is case-sensitive"
        return
    fi
    # cygpath keeps the case it was given (measured), so the two spellings
    # below are two different strings that only norm_path's case fold can
    # bring to one key — drop that step and this case turns red. Leg B uses
    # the short -B flag too, so that flag has a guard beyond the print case.
    local absA absB
    absA=$(cygpath -ma "$dir")
    absB=$(printf '%s' "$absA" | tr 'A-Z' 'a-z')
    if [ "$absA" = "$absB" ]; then
        # Nothing to reconcile would make this a second copy of A1, so a
        # broken fold could pass unnoticed; say so instead of going green.
        skip "$case_name" "the two spellings are one string ($absA)"
        return
    fi
    one_tree_pair a2A a2B "$WS/a2.events" --build "$absA" -B "$absB"
}

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

run_case a1-same-tree-spellings case_a1_same_tree_spellings
run_case a2-same-tree-case case_a2_same_tree_case
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
