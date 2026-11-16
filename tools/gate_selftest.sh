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

# The key-to-directory mapping A3 builds in its mapping window, kept for A4,
# whose argv has to be written before its own legs hold the locks. One key and
# one path per line, in the same order (the lock key is the claim directory's
# name, spec D-8). Empty until A3 has seen both claims.
a3_keys=""
a3_paths=""

# release_all_legs: hand out every GO still owed, and forget about them.
release_all_legs() {
    local go
    for go in ${pending_gos[@]+"${pending_gos[@]}"}; do
        [ -e "$go" ] || touch "$go"
    done
    pending_gos=()
}

cleanup() {
    # A leg whose case failed is still waiting for its GO; hand it out, then
    # wait, so no stub process outlives the workspace it writes into.
    release_all_legs
    wait
    rm -rf "$WS"
}
trap cleanup EXIT

failures=0

pass() { printf 'selftest: PASS %s\n' "$1"; }

# A failed case leaves its legs parked on their GOs, and a parked leg keeps the
# write-directory lock it claimed. The next case then counts that lock among its
# own holders and fails for a reason that belongs to the previous case, so a
# single defect would burn the whole budget. Draining the legs here keeps a red
# run to exactly the case that is red.
fail() {
    printf 'selftest: FAIL %s: %s\n' "$1" "$2"
    failures=$(( failures + 1 ))
    release_all_legs
    wait
}

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

# leg_env <name> <stem> <go> <dir> <command...>: as leg, but the write directory
# reaches gate.sh through the environment instead of argv. The assignment has to
# sit on the same line as the command — exporting it would re-point every later
# gate on the machine (spec D-3) — which is why this cannot go through leg().
leg_env() {
    local name="$1" stem="$2" go="$3" dir="$4"
    shift 4
    if [ -n "$go" ]; then
        pending_gos+=("$go")
    fi
    NLANG_GATE_WRITE_DIR="$dir" bash "$GATE_SH" --shared "$name" "$stem" "$@" > "$stem.console" 2>&1 &
}

# holder_records <count>: poll until exactly <count> write-directory holder
# files exist, then print one line per holder as <key>, <name>, <path>,
# tab-separated. The driver reads the key-to-directory pairing out of the
# holder files rather than re-running the normalization recipe itself: a second
# copy of that recipe in the test would turn red twice over whenever the
# implementation changes, and a green run would then prove nothing about the
# pairing (spec 八.2). The key half is what a later case needs to order its own
# argv, and the name= half says which leg made which claim, which is the only
# way to tell a claim on the right directory from a claim on some other one.
# find instead of a glob, because an unmatched glob stays literal and prints
# its own no-such-file line into the result.
holder_records() {
    local want="$1" n files f key name path
    while :; do
        files=$(find "$WS/lock/wdir" -mindepth 2 -maxdepth 2 -name holder.info -print 2>/dev/null)
        n=$(printf '%s\n' "$files" | grep -c .)
        [ "$n" -eq "$want" ] && break
        [ "$(deadline)" -gt 0 ] || return 1
        sleep "$NLANG_GATE_POLL_S"
    done
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        # The directory the lock was claimed under is named by the key (spec
        # D-8), so the parent directory's name is the key itself.
        key=$(basename "$(dirname "$f")")
        name=$(sed -n 's/^name=//p' "$f" | head -1)
        path=$(sed -n 's/^path=//p' "$f" | head -1)
        printf '%s\t%s\t%s\n' "$key" "$name" "$path"
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
    local goA goB records holder_path want first
    goA=$(go_file "$case_name" A)
    goB=$(go_file "$case_name" B)
    leg "$stemA" "$stemA" "$goA" bash "$SELF" --stub A "$ev" "$goA" "$flagA" "$dirA"
    if ! await "$ev" "start A"; then
        fail "$case_name" "leg A never started: events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    if ! records=$(holder_records 1); then
        fail "$case_name" "no single write-directory holder appeared: holders=[$(holder_dump)] events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    holder_path=${records##*$'\t'}
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

case_a3_different_trees() {
    # Two legs writing two different trees have to overlap: keying the lock by
    # directory rather than by machine is the whole point of the mechanism. Leg
    # A names its tree with --build and leg B with --site-dir, because those are
    # the flags the real commands use (cmake --build, nlang_docs --site-dir) and
    # the mutation checks delete one arm at a time — pin the spelling here or a
    # deleted arm has no case it is supposed to turn red.
    local dirA="$WS/t1" dirB="$WS/t2"
    mkdir -p "$dirA" "$dirB"
    local nameA=a3A nameB=a3B stemA=a3A stemB=a3B
    local ev="$WS/a3.events" goA goB records n key="" name="" path=""
    local keyA="" keyB="" pathA="" pathB="" waitA waitB acqA acqB head2 tail2
    goA=$(go_file "$case_name" A)
    goB=$(go_file "$case_name" B)
    leg "$nameA" "$stemA" "$goA" bash "$SELF" --stub A "$ev" "$goA" --build "$dirA"
    if ! await "$ev" "start A"; then
        fail "$case_name" "leg A never started: events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    leg "$nameB" "$stemB" "$goB" bash "$SELF" --stub B "$ev" "$goB" --site-dir "$dirB"
    if ! await "$ev" "start B"; then
        fail "$case_name" "leg B never started: console=[$(cat "$stemB.console" 2>/dev/null)] holders=[$(holder_dump)] events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    # The mapping window: both legs are inside their stubs, no GO has been handed
    # out, so this is the only moment the case can see both claims at once. The
    # two key names go to the driver's variables here rather than being listed
    # again by the case that needs them, because that case has to write its
    # reverse-ordered argv before its own legs hold the locks.
    if ! records=$(holder_records 2); then
        fail "$case_name" "expected two write-directory holders, holders=[$(holder_dump)] events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    n=0
    while IFS=$'\t' read -r key name path; do
        [ -n "$key" ] || continue
        n=$(( n + 1 ))
        # Each leg claimed its own tree. The holder count alone would not show a
        # flag arm wired to the wrong directory: a wrong but distinct directory
        # still gives two holders and still lets the legs run at the same time,
        # so only this pairing turns that write red. It fails together with the
        # count half when an arm is deleted outright (then leg B claims nothing
        # at all), and that joint failure is what makes the pair discriminating.
        case "$name:$path" in
            "$nameA":*/t1) keyA="$key"; pathA="$path" ;;
            "$nameB":*/t2) keyB="$key"; pathB="$path" ;;
            *) fail "$case_name" "holder $key says name=$name path=$path, expected $nameA for $dirA and $nameB for $dirB"; return ;;
        esac
    done <<< "$records"
    if [ "$n" -ne 2 ] || [ -z "$keyA" ] || [ -z "$keyB" ]; then
        fail "$case_name" "expected one holder per leg, found $n: records=[$(printf '%s\n' "$records" | tr '\n' ';')]"
        return
    fi
    a3_keys="$keyA"$'\n'"$keyB"
    a3_paths="$pathA"$'\n'"$pathB"
    # Neither leg waited: the two claims are for different keys. The negative
    # half is anchored on the directory lock's own first-round wording, because a
    # machine-lock wait line would fail it for the wrong reason and the repeat
    # lines look the same for both locks. The acquired lines are asserted as
    # well: a leg that claimed no directory also prints no wait line.
    waitA=$(count_lines "$stemA.console" "waiting for the write-directory lock on")
    waitB=$(count_lines "$stemB.console" "waiting for the write-directory lock on")
    acqA=$(count_lines "$stemA.console" "write-directory lock acquired after")
    acqB=$(count_lines "$stemB.console" "write-directory lock acquired after")
    if [ "$waitA" -ne 0 ] || [ "$waitB" -ne 0 ]; then
        fail "$case_name" "a different-tree leg waited: A=$waitA B=$waitB wait lines"
        return
    fi
    if [ "$acqA" -ne 1 ] || [ "$acqB" -ne 1 ]; then
        fail "$case_name" "expected one directory-lock acquired line per leg, found A=$acqA B=$acqB"
        return
    fi
    # Each leg's acquired line has to name the directory its own holder recorded
    # (spec 七: the directory in the log is the normalized path, the same string
    # as the holder's path= line). Comparing the two is how the test checks that
    # without copying the normalization recipe into itself.
    if [ "$(count_lines "$stemA.console" "($pathA)")" -ne 1 ] ||
       [ "$(count_lines "$stemB.console" "($pathB)")" -ne 1 ]; then
        fail "$case_name" "a leg acquired a directory other than its holder's: A=[$pathA] B=[$pathB]"
        return
    fi
    release_leg "$goA"
    release_leg "$goB"
    if ! await "$stemA.console" "GATE $nameA: PASS" || ! await "$stemB.console" "GATE $nameB: PASS"; then
        fail "$case_name" "a leg never printed its verdict: A=[$(cat "$stemA.console" 2>/dev/null)] B=[$(cat "$stemB.console" 2>/dev/null)]"
        return
    fi
    # Both starts before either finish, in either order within each pair: that is
    # "two trees do not serialize", stated about the event file rather than about
    # any timing.
    head2=$(head -n 2 "$ev" 2>/dev/null | LC_ALL=C sort | tr '\n' ';')
    tail2=$(tail -n 2 "$ev" 2>/dev/null | LC_ALL=C sort | tr '\n' ';')
    if [ "$(count_lines "$ev" "start ")" -ne 2 ] ||
       [ "$(count_lines "$ev" "finish ")" -ne 2 ] ||
       [ "$head2" != "start A;start B;" ] ||
       [ "$tail2" != "finish A;finish B;" ]; then
        fail "$case_name" "legs not overlapping: events=[$(tr '\n' ';' < "$ev" 2>/dev/null)]"
        return
    fi
    pass "$case_name"
}

case_a4_two_tree_vs_single() {
    # One leg claiming two directories, against one leg claiming one of them. The
    # two-directory leg's argv names its directories in the reverse of the claim
    # order this case expects, which is the crossing shape no single leg can
    # resolve by itself: only an order every leg shares settles it (spec D-5).
    if [ -z "$a3_keys" ] || [ -z "$a3_paths" ]; then
        fail "$case_name" "A3 left no key-to-directory mapping to order the argv by"
        return
    fi
    # Which key comes first is read off the keys themselves, never assumed from the
    # t1/t2 labels: a key is the first 16 hex digits of a hash over a mktemp
    # directory name, so which tree wins is a coin flip, and assuming it would
    # make the reverse-ordered argv below equal to the sorted order whenever the
    # assumption missed — the case would then be a no-op on those runs. LC_ALL=C is
    # also the comparison basis of the sort in acquire_wdirs, so the "reverse
    # order" premise cannot flip under some other locale.
    local -a a4_keys=() a4_paths=()
    mapfile -t a4_keys <<< "$a3_keys"
    mapfile -t a4_paths <<< "$a3_paths"
    if [ "${#a4_keys[@]}" -ne 2 ] || [ "${#a4_paths[@]}" -ne 2 ]; then
        fail "$case_name" "expected two mapped keys and paths, found ${#a4_keys[@]} and ${#a4_paths[@]}"
        return
    fi
    local early_key late_key early_dir late_dir first
    if [ "${a4_keys[0]}" = "${a4_keys[1]}" ]; then
        # Two holders means two distinct keys, so this cannot happen while A3
        # passed; said anyway, because equal keys would make the order below
        # unorderable rather than red.
        fail "$case_name" "A3's two lock keys are equal (${a4_keys[0]}): nothing to order"
        return
    fi
    first=$(printf '%s\n' "${a4_keys[0]}" "${a4_keys[1]}" | LC_ALL=C sort | head -1)
    if [ "$first" = "${a4_keys[0]}" ]; then
        early_key="${a4_keys[0]}"; late_key="${a4_keys[1]}"
        early_dir="${a4_paths[0]}"; late_dir="${a4_paths[1]}"
    else
        early_key="${a4_keys[1]}"; late_key="${a4_keys[0]}"
        early_dir="${a4_paths[1]}"; late_dir="${a4_paths[0]}"
    fi
    local nameM=a4M stemM=a4M nameS=a4S stemS=a4S
    local ev="$WS/a4.events" goM goS want acqM early_line late_line
    local want_events="start multi
finish multi
start single
finish single"
    goM=$(go_file "$case_name" multi)
    goS=$(go_file "$case_name" single)
    # Reverse-ordered argv: the later key's directory is named first, so a claim
    # order that just follows argv would claim late-then-early.
    leg "$nameM" "$stemM" "$goM" bash "$SELF" --stub multi "$ev" "$goM" --build "$late_dir" --site-dir "$early_dir"
    # The stub starts only once both directory locks are held, so this await is
    # what makes the two acquired lines read below already exist.
    if ! await "$ev" "start multi"; then
        fail "$case_name" "the two-directory leg never started: console=[$(cat "$stemM.console" 2>/dev/null)] holders=[$(holder_dump)]"
        return
    fi
    # The single-tree leg asks for the directory the two-directory leg claimed
    # last, so it has to queue. That wait line is this case's precondition, as in
    # the env case: waiting for it before handing out any GO is what keeps the
    # serialization order below from depending on how busy the machine is. The
    # --build spelling is pinned here as well, so that deleting that flag arm
    # turns this leg red instead of leaving it running unlocked by luck.
    leg "$nameS" "$stemS" "$goS" bash "$SELF" --stub single "$ev" "$goS" --build "$late_dir"
    want="LOCK $nameS: waiting for the write-directory lock on $late_dir ("
    if ! await "$stemS.console" "$want"; then
        fail "$case_name" "the single-tree leg never printed the write-directory wait line for $late_dir: console=[$(cat "$stemS.console" 2>/dev/null)] holders=[$(holder_dump)]"
        return
    fi
    release_leg "$goM"
    if ! await "$ev" "finish multi" || ! await "$stemM.console" "GATE $nameM: PASS"; then
        fail "$case_name" "the two-directory leg never finished: console=[$(cat "$stemM.console" 2>/dev/null)]"
        return
    fi
    # The claim order, read off the two acquired lines in the order they were
    # printed: ascending lock key. The "write-directory lock acquired" wording
    # keeps the machine lock's own acquired line out of the count, and each
    # directory is named by exactly one of the two lines.
    acqM=$(count_lines "$stemM.console" "write-directory lock acquired after")
    early_line=$(grep -n -F -m 1 -- "($early_dir)" "$stemM.console" 2>/dev/null | cut -d: -f1)
    late_line=$(grep -n -F -m 1 -- "($late_dir)" "$stemM.console" 2>/dev/null | cut -d: -f1)
    if [ "$acqM" -ne 2 ]; then
        fail "$case_name" "expected two directory-lock acquired lines, found $acqM: console=[$(cat "$stemM.console" 2>/dev/null)]"
        return
    fi
    if [ -z "$early_line" ] || [ -z "$late_line" ]; then
        fail "$case_name" "an acquired line is missing for one of the two directories: early=[$early_dir at line ${early_line:-none}] late=[$late_dir at line ${late_line:-none}]"
        return
    fi
    if [ "$early_line" -ge "$late_line" ]; then
        fail "$case_name" "claims are not in ascending lock-key order: key=$early_key ($early_dir) at line $early_line, key=$late_key ($late_dir) at line $late_line"
        return
    fi
    release_leg "$goS"
    if ! await "$ev" "finish single" || ! await "$stemS.console" "GATE $nameS: PASS"; then
        fail "$case_name" "the single-tree leg never finished: console=[$(cat "$stemS.console" 2>/dev/null)]"
        return
    fi
    if [ "$(cat "$ev" 2>/dev/null)" != "$want_events" ]; then
        fail "$case_name" "legs not serialized: events=[$(tr '\n' ';' < "$ev" 2>/dev/null)]"
        return
    fi
    pass "$case_name"
}

case_a5_env_write_dir() {
    # One directory reached two ways: leg A through argv, leg B through an inline
    # NLANG_GATE_WRITE_DIR with no flag at all. Claims are keyed by the
    # normalized path, so the two sources have to collide; were the environment
    # channel ignored, leg B would run unlocked and both legs would write one
    # tree at once.
    local dir="$WS/t3"
    mkdir -p "$dir"
    local nameA=a5A nameB=a5B stemA=a5A stemB=a5B
    local ev="$WS/a5.events" goA goB records holder_path want
    goA=$(go_file "$case_name" A)
    goB=$(go_file "$case_name" B)
    leg "$nameA" "$stemA" "$goA" bash "$SELF" --stub A "$ev" "$goA" --build "$dir"
    if ! await "$ev" "start A"; then
        fail "$case_name" "leg A never started: events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    if ! records=$(holder_records 1); then
        fail "$case_name" "no write-directory holder for leg A: holders=[$(holder_dump)] events=[$(cat "$ev" 2>/dev/null)]"
        return
    fi
    holder_path=${records##*$'\t'}
    leg_env "$nameB" "$stemB" "$goB" "$dir" bash "$SELF" --stub B "$ev" "$goB"
    # This wait line is the case's precondition, not one of its assertions: the
    # stub only finishes on its GO, so leg B necessarily started while A still
    # held the directory, and a working directory lock has to print this line.
    # Waiting for it before releasing A is what keeps the case from degrading
    # into something that depends on how busy the machine is.
    want="LOCK $nameB: waiting for the write-directory lock on $holder_path ("
    if ! await "$stemB.console" "$want"; then
        fail "$case_name" "leg B never printed the write-directory wait line for $holder_path: console=[$(cat "$stemB.console" 2>/dev/null)] holders=[$(holder_dump)]"
        return
    fi
    release_leg "$goA"
    if ! await "$ev" "finish A" || ! await "$stemA.console" "GATE $nameA: PASS"; then
        fail "$case_name" "leg A never finished: console=[$(cat "$stemA.console" 2>/dev/null)]"
        return
    fi
    release_leg "$goB"
    if ! await "$ev" "finish B" || ! await "$stemB.console" "GATE $nameB: PASS"; then
        fail "$case_name" "leg B never finished: console=[$(cat "$stemB.console" 2>/dev/null)]"
        return
    fi
    # Leg B's own acquired line has to name the directory A's holder recorded:
    # the two sources produced one key. The closing parenthesis keeps this off
    # the wait line, which ends "<dir> (<holder>)" instead.
    if [ "$(count_lines "$stemB.console" "($holder_path)")" -ne 1 ]; then
        fail "$case_name" "leg B acquired a directory other than $holder_path: console=[$(cat "$stemB.console" 2>/dev/null)]"
        return
    fi
    local want_events="start A
finish A
start B
finish B"
    if [ "$(cat "$ev" 2>/dev/null)" != "$want_events" ]; then
        fail "$case_name" "legs not serialized: events=[$(tr '\n' ';' < "$ev" 2>/dev/null)]"
        return
    fi
    pass "$case_name"
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
run_case a3-different-trees case_a3_different_trees
run_case a4-two-tree-vs-single case_a4_two_tree_vs_single
run_case a5-env-write-dir case_a5_env_write_dir
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
