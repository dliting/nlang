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
wdir_dir="$lock_dir/wdir"
# The keys whose directory locks were actually claimed, one per line in claim
# order, so that the EXIT trap releases exactly what was taken and nothing that
# was still being waited for (spec D-5).
wdir_held=""

usage() {
    echo "usage: tools/gate.sh [--shared] <name> <logfile-stem> <command> [args...]" >&2
    exit 64
}

# --- write-directory derivation ---------------------------------------------
#
# The lock resource is the set of directories a gate command names for
# itself: the build tree it writes, the test tree it writes, the docs site
# directory it writes. Reading them out of the flags the caller already had
# to type is what makes the lock say something true about the command, and
# it keeps one spelling recipe shared by the lock and by --print-write-dirs.

# Absolute path in the form the machine's own tools agree on. The choice is
# made on tool availability rather than a guessed system name: cygpath
# exists exactly where the Windows path rules apply. The case fold belongs
# to that family only — NTFS is case-insensitive, so two spellings of one
# directory have to converge, while POSIX is case-sensitive and a fold would
# merge two directories that really are different. gate.sh never cds, so a
# relative directory resolves against $PWD, the same directory the wrapped
# command sees.
norm_path() {
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -ma "$1" | tr 'A-Z' 'a-z'
    else
        realpath -m "$1"
    fi
}

# Lock directory name for one normalized path: a hash because a path holds
# characters a directory name cannot, 16 hex digits because that is short
# enough to read in a log and far beyond the handful of lock directories one
# machine has. The path itself goes into the holder file, so the hash is
# never the only way to find out which directory a lock covers.
wdir_key() {
    printf '%s' "$1" | sha256sum | cut -c1-16
}

# One normalized directory per line, in order of first appearance, keeping
# the first of any duplicates. Every spelling counts: the separated flag,
# the '=' form, and the short -B with or without its glue. A flag with no
# value after it and an empty value are both skipped rather than normalized:
# neither yields a path (cygpath and realpath both fail on the empty
# string), and falling through would either lock the leg's own working
# directory or put unrelated broken calls behind one key computed from
# nothing. NLANG_GATE_WRITE_DIR is the third source, for a caller that
# cannot add a flag to its command line; it holds one directory, because
# several directories already have the command line.
derive_write_dirs() {
    local raw
    {
        while [ "$#" -gt 0 ]; do
            raw=""
            case "$1" in
                --build|--test-dir|--site-dir|--workdir|-B)
                    if [ "$#" -ge 2 ]; then
                        raw="$2"
                        shift
                    fi
                    ;;
                --build=*|--test-dir=*|--site-dir=*|--workdir=*)
                    raw="${1#*=}"
                    ;;
                -B*)
                    # -B=DIR reads as the literal DIR: a broken call, still
                    # worth locking rather than running unlocked.
                    raw="${1#-B}"
                    raw="${raw#=}"
                    ;;
            esac
            shift
            if [ -n "$raw" ]; then
                norm_path "$raw"
            fi
        done
        if [ -n "${NLANG_GATE_WRITE_DIR-}" ]; then
            norm_path "$NLANG_GATE_WRITE_DIR"
        fi
    } | awk '!seen[$0]++'
}

shared_mode=0
if [ "${1:-}" = "--shared" ]; then
    shared_mode=1
    shift
fi

# Key dump for the selftest and for the configure-time probe: no name, no
# stem, and no lock of any kind, so configuring a build tree never queues
# behind a gate batch. The keys are printed here rather than left for each
# caller to hash, which would put the normalization recipe in several files.
if [ "${1:-}" = "--print-write-dirs" ]; then
    shift
    derive_write_dirs "$@" | while read -r wdir_path; do
        printf 'key %s path %s\n' "$(wdir_key "$wdir_path")" "$wdir_path"
    done
    exit 0
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
        # A directory lock names its flag directory by a hash, so this file is
        # the one place where that key and the real path are recorded together
        # (spec D-8). The machine lock has no path of its own, so it gets no
        # path= line and its holders keep the shape they have today.
        if [ "$#" -ge 2 ]; then
            printf 'path=%s\n' "$2"
        fi
    } > "$1"
}

# The holder of one directory lock: write_holder's fields plus the path= line.
wdir_holder() { # <holder-file> <normalized-dir>
    write_holder "$1" "$2"
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

# The one age source for every lock's remnants. holder_age_s covers the case
# where the holder file was written and is readable; this covers the other
# half, which is exactly the half a killed process leaves: no holder file at
# all (killed between the atomic mkdir and the write that follows), or one cut
# off before the started= line. Both locks have that two-step shape, so both
# need it, and reading the age in several places is how the two would drift
# apart — a drift that only shows up the moment a gate is killed, which no
# ordinary test run produces.
#
# The second argument names the object the flag itself was stood on, which is
# where the creation time still is recorded when the holder file is not: the
# flag directory where one is created before the holder is written, the holder
# file itself where the file IS the flag. Nothing readable at all (a lock
# cleaned up between the check and this call) ages as 0, the same answer
# holder_age_s gives when it has nothing to go on.
holder_age_or_mtime_s() {
    local age stamp
    age=$(holder_age_s "$1")
    if [ "$age" -gt 0 ]; then
        echo "$age"
        return
    fi
    # A holder that says nothing: read the flag object's own clock instead.
    # A holder written this very second also lands here, and its flag object
    # was created just as recently, so the answer is the same.
    stamp=$(stat -c %Y "$2" 2>/dev/null)
    if [ -n "$stamp" ]; then
        echo $(( $(date +%s) - stamp ))
    else
        echo 0
    fi
}

# Drop holders that outlived their process (a gate killed with the shell
# closed leaves its directory behind, and that must not freeze the
# machine until someone notices).
sweep_stale() {
    if [ -d "$excl_dir" ]; then
        age=$(holder_age_or_mtime_s "$excl_dir/holder.info" "$excl_dir")
        if [ "$age" -gt "$LOCK_STALE_S" ]; then
            echo "LOCK $name: taking over an exclusive lock held ${age}s by $(holder_text "$excl_dir/holder.info")"
            rm -rf "$excl_dir"
        fi
    fi
    for file in "$shared_dir"/*.info; do
        [ -f "$file" ] || continue
        age=$(holder_age_or_mtime_s "$file" "$file")
        if [ "$age" -gt "$LOCK_STALE_S" ]; then
            echo "LOCK $name: dropping a shared lock held ${age}s by $(holder_text "$file")"
            rm -f "$file"
        fi
    done
}

# Take over a directory lock whose gate is gone. The fallback half of the age
# source is not a detail here: the flag directory is created before its holder
# is written, so a gate killed between those two steps leaves a flag with no
# started= line at all, which on started= alone would stay zero seconds old
# forever and keep everyone else waiting.
sweep_stale_wdir() { # <key> <normalized-dir>
    local age
    age=$(holder_age_or_mtime_s "$wdir_dir/$1/holder.info" "$wdir_dir/$1")
    if [ "$age" -gt "$LOCK_STALE_S" ]; then
        echo "LOCK $name: taking over the write-directory lock on $2 held ${age}s by $(holder_text "$wdir_dir/$1/holder.info")"
        rm -rf "$wdir_dir/$1"
    fi
}

# Report the current holder at most once a minute while waiting. The caller
# brings the first-round line, because that line is the only place the two locks
# differ: the machine lock names its lock directory there (which is what shows up
# when two batches silently took two different locks), the write-directory lock
# names the directory it wants, and the repeat lines read the same for both. The
# throttle itself lives here and nowhere else, so a wait can report at one rate
# whatever lock it is waiting on.
wait_round() { # <waited-seconds> <first-round-line> <holder-description>
    if [ "$1" -eq 0 ]; then
        echo "$2"
    elif [ $(( $1 % LOCK_REPORT_EVERY_S )) -eq 0 ]; then
        echo "LOCK $name: still waiting ${1}s ($3)"
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
            desc="exclusive lock held $(holder_age_or_mtime_s "$excl_dir/holder.info" "$excl_dir")s by $(holder_text "$excl_dir/holder.info")"
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
                desc="exclusive lock held $(holder_age_or_mtime_s "$excl_dir/holder.info" "$excl_dir")s by $(holder_text "$excl_dir/holder.info")"
            fi
        fi
        wait_round "$waited" "LOCK $name: waiting for the machine lock in $lock_dir ($desc)" "$desc"
        sleep "$LOCK_POLL_S"
        waited=$(( waited + LOCK_POLL_S ))
    done
}

# Take one directory lock per directory the command names for itself, so two legs
# writing one tree queue while legs writing different trees keep running side by
# side. Only build gates ask: the machine lock already keeps every test gate out
# of every build gate's way, so a test gate has nothing left to conflict with.
acquire_wdirs() {
    local wdirs wdir_path key desc waited
    if [ "$shared_mode" -ne 1 ]; then
        return 0
    fi
    wdirs=$(derive_write_dirs "$@")
    # A build gate that names no directory at all cannot be checked for a
    # conflicting write target, and running it silently unlocked is how two
    # batches end up in one tree again. Warning rather than failing is what keeps
    # the directory-less legs that are legitimate today running.
    if [ -z "$wdirs" ]; then
        echo "LOCK $name: no write directory derived from the command line, no directory lock taken"
        return 0
    fi
    while IFS= read -r wdir_path; do
        [ -n "$wdir_path" ] || continue
        key=$(wdir_key "$wdir_path")
        waited=0
        while :; do
            # Re-made every round, for the same reason acquire() re-makes the lock
            # root: a temp cleaner can take either parent away while we wait, and
            # without this the claim below would fail forever. The claim is a bare
            # mkdir because mkdir -p also reports success for a directory that
            # already exists, and reporting that failure is the whole mutual
            # exclusion.
            mkdir -p "$lock_dir" "$wdir_dir"
            sweep_stale_wdir "$key" "$wdir_path"
            if mkdir "$wdir_dir/$key" 2>/dev/null; then
                wdir_holder "$wdir_dir/$key/holder.info" "$wdir_path"
                wdir_held+="$key"$'\n'
                echo "LOCK $name: write-directory lock acquired after ${waited}s ($wdir_path)"
                break
            fi
            # The same wording and the same age source as the machine lock's wait
            # line, so a holder read through either lock says the same thing.
            desc="write-directory lock held $(holder_age_or_mtime_s "$wdir_dir/$key/holder.info" "$wdir_dir/$key")s by $(holder_text "$wdir_dir/$key/holder.info")"
            wait_round "$waited" "LOCK $name: waiting for the write-directory lock on $wdir_path ($desc)" "$desc"
            sleep "$LOCK_POLL_S"
            waited=$(( waited + LOCK_POLL_S ))
        done
    done <<< "$wdirs"
}

release() {
    # Directory locks first: releasing runs in the reverse of the order things
    # were taken, and the machine lock is the outermost of them.
    release_wdirs
    if [ "$shared_mode" -eq 1 ]; then
        rm -f "$shared_dir/$name-$$.info"
    else
        rm -rf "$excl_dir"
    fi
    # Only the holder files go away; the root stays, so the atomic claim
    # always has its parent to work in.
    rmdir "$shared_dir" 2>/dev/null || true
}

# Keys in the reverse of the order they were claimed, which is what keeps the
# claims from ever forming a cycle a waiting leg cannot get out of. Only the
# holder and its flag directory go away; wdir/ and the lock root stay, as in
# release().
release_wdirs() {
    local key
    while IFS= read -r key; do
        [ -n "$key" ] || continue
        rm -f "$wdir_dir/$key/holder.info"
        rmdir "$wdir_dir/$key" 2>/dev/null || true
    done < <(printf '%s' "$wdir_held" | tac)
}

mkdir -p "$lock_dir"
acquire
# Armed before the directory locks are taken, so a signal arriving while one is
# being waited on releases the directories already claimed instead of leaving
# them for the next gate to age out.
trap release EXIT
acquire_wdirs "$@"

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
