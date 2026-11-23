#!/usr/bin/env bash
# gate.sh — run one gate command and report its REAL exit code.
#
# Why this exists: `ctest ... | tail` reports the pipe's exit code
# (tail's, always 0), which once turned a 4-failure ctest run into a
# recorded "all green". Every gate (ctest, e2e, docs builds) must run
# through this script instead of a piped tail.
#
# Usage: tools/gate.sh [--claim <resource>]... <name> <logfile-stem> <command> [args...]
#        tools/gate.sh --print-write-dirs <command> [args...]
#   - stdout and stderr are captured SEPARATELY to <stem>.out/.err
#     (merged capture can interleave and drop lines).
#   - The command runs without a pipe; its own exit code is final.
#   - Prints both tails, then one verdict line: "GATE <name>: PASS" or
#     "GATE <name>: FAIL rc=<n>". The script exits with that rc.
# Read a gate result ONLY from the verdict line or the exit code —
# never from a tail alone. Wait/report lines use a "LOCK " prefix, so
# they can never be mistaken for a verdict.
#
# Locks: two kinds, each naming something real about the collision it
# prevents (gate timing design D-10 retired the machine-wide lock that used
# to sit above both, and the --shared flag that asked for it: what it guarded
# — %TEMP% collisions, the desktop focus chain, timing sensitivity — is each
# handled in its own layer now, and the lock itself only ever queued legs
# that share nothing).
#
#   Write-directory lock: every gate claims every directory its own command
#             names (--build/-B, --test-dir, --site-dir, --workdir, or an
#             inline NLANG_GATE_WRITE_DIR), because a build tree's CPack and
#             CMakeCache files sit at its root and are shared by every
#             configuration — two configurations of one tree building at once
#             fight over the same .tmp files. Claims are keyed by the
#             normalized path, taken with an atomic mkdir of wdir/<key>/
#             under the lock directory, and released in the reverse order
#             they were taken; a leg that holds several claims takes them in
#             ascending key order, which is what keeps two such legs from
#             deadlock. The stale rule is age only: a remnant older than
#             LOCK_STALE_S is taken over whether or not its holder is still
#             alive, and the holder's pid= line is written for a human to
#             read — nothing checks it, because an MSYS pid says nothing
#             about liveness across a reboot. A directory claimed by a leg
#             that was killed therefore still waits out that same threshold;
#             when that is too long, the waiting line prints the holder (its
#             path= names the directory), the claim can be removed by hand,
#             or NLANG_GATE_STALE_S lowers the threshold. A gate that derives
#             no writable directory and names no claim prints a warning and
#             runs unlocked, and one whose digest tool printed no hash for a
#             directory it did derive warns the same way rather than claiming
#             a lock under an unusable name.
#
#   Claim lock: --claim <resource> (repeatable) takes a named-resource lock
#             at $lock_dir/claim/<resource>/, with the same two-step
#             atomic-mkdir-then-holder shape and the same age-based takeover
#             as the write-directory lock. It serializes gates that must not
#             run beside each other but share no write directory — today the
#             nide-gui claim over the ctest legs of trees that register
#             window-launching tests (gate timing design D-11), where ctest's
#             own RESOURCE_LOCK covers the entries inside one process and the
#             claim covers whole legs running side by side. Two gates naming
#             the same claim queue one at a time; different names overlap.
#
# Acquisition order is fixed for every gate: claims in ascending
# resource-name order, then write-directory locks in ascending key order;
# release runs the exact reverse. One total order across the two kinds is
# the same sufficient condition wdir_keyed_pairs pins for directories, so
# claims can never form a hold-one-wait-the-other cycle against each other
# or against the directory locks.
#
# The lock lives in $TMPDIR/nlang-gate-<user>/ by default (one directory
# per machine user, shared by every worktree); set NLANG_GATE_LOCK_DIR to
# point it somewhere else, which a CI runner with a per-job TMPDIR has to
# do to keep one machine's gates in one lock. The wait lines print where
# they wait — a claim says which lock root, a write-directory lock says
# which directory — so a batch that took the wrong lock is visible in the
# log rather than silent.
#
# --print-write-dirs runs that same derivation and prints one
# "key <16 hex> path <dir>" line per derived directory: no leg name, no lock of
# any kind, no output files, so a configure-time probe can read the keys without
# queueing behind a batch. It is the only place the normalization recipe lives
# for a caller; hashing the paths itself would put a second copy of the recipe
# in that caller's file.
#
# Env overrides, for the probes in temp/ and for CI:
#   NLANG_GATE_LOCK_DIR (the lock directory itself), NLANG_GATE_POLL_S,
#   NLANG_GATE_REPORT_EVERY_S, NLANG_GATE_STALE_S,
#   NLANG_GATE_WRITE_DIR (force the write directory of one leg, as an
#   inline prefix only — exporting it would silently re-point every later
#   gate on the machine), NLANG_GATE_SH (read only by the selftest driver:
#   the switch that points it at a mutated gate.sh).

set -u

LOCK_POLL_S=${NLANG_GATE_POLL_S:-5}
LOCK_REPORT_EVERY_S=${NLANG_GATE_REPORT_EVERY_S:-60}
LOCK_STALE_S=${NLANG_GATE_STALE_S:-10800}

# USER is set but empty in some MSYS shells, so fall through on empty
# values rather than locking under an anonymous name.
lock_owner="${USER:-${USERNAME:-unknown}}"
lock_dir="${NLANG_GATE_LOCK_DIR:-${TMPDIR:-/tmp}/nlang-gate-$lock_owner}"
wdir_dir="$lock_dir/wdir"
claim_root="$lock_dir/claim"
# The keys whose directory locks were actually claimed, one per line in claim
# order, so that the EXIT trap releases exactly what was taken and nothing that
# was still being waited for (spec D-5). The claim names follow the same rule
# for the resource locks.
wdir_held=""
claim_held=""

usage() {
    echo "usage: tools/gate.sh [--claim <resource>]... <name> <logfile-stem> <command> [args...]" >&2
    echo "       tools/gate.sh --print-write-dirs <command> [args...]" >&2
    echo "           prints the derived write-directory keys; takes no lock and" >&2
    echo "           writes no file, so a configure-time probe can run in a batch" >&2
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

# Take the normalized directories on stdin, one per line, and re-emit each as
# "key<TAB>path" in ascending key order. Every leg claims in this order whatever
# its command line happened to name first, and one total order shared by every
# leg is the sufficient condition for never deadlocking: two legs whose commands
# name the same two directories in different orders would otherwise each claim
# one and then wait for the other (spec D-5). LC_ALL=C pins the comparison to
# byte order, so the claim order does not depend on the locale a gate happens to
# run under either.
wdir_keyed_pairs() {
    local wdir_path
    while IFS= read -r wdir_path; do
        [ -n "$wdir_path" ] || continue
        printf '%s\t%s\n' "$(wdir_key "$wdir_path")" "$wdir_path"
    done | LC_ALL=C sort
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

# Leading options only, so they stay separable from the wrapped command's own
# argv (a --claim after the name belongs to the command, not to the lock).
claims=""
while [ "${1:-}" = "--claim" ]; do
    shift
    if [ $# -lt 1 ]; then
        usage
    fi
    # The name becomes a directory name under the lock root, so it is checked
    # against the same charset the roster and gate_tier.sh use: the claim is
    # named, not hashed, and a name carrying a slash or whitespace would put
    # the lock somewhere else or nowhere.
    case "$1" in
        ""|.|..) usage ;;
        *[!A-Za-z0-9_.-]*) usage ;;
    esac
    # Duplicate names collapse: one resource, one lock.
    case "$claims" in
        *" $1 "*) ;;
        *) claims="$claims $1 " ;;
    esac
    shift
done

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

# A leading token that still looks like an option after the known ones were
# consumed is a stale or mistyped invocation. gate.sh once took --shared
# here, and silently eating a removed option would start the wrapped command
# one word late — a misparse no verdict line would explain — so an unknown
# leading option now fails with usage instead.
if [ -n "${1:-}" ] && [ "${1#-}" != "$1" ]; then
    echo "gate.sh: unrecognized leading option: $1" >&2
    usage
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
        printf 'cmd=%s\n' "$gate_cmd"
        # A directory lock names its flag directory by a hash, so this file is
        # the one place where that key and the real path are recorded together
        # (spec D-8).
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

# Take over a claim whose gate is gone. The same two-step shape as the
# directory lock (the claim directory is created before its holder is
# written), so the same fallback age source is what stops a claim killed
# between those two steps from keeping everyone else waiting.
sweep_stale_claim() { # <claim-name>
    local age
    age=$(holder_age_or_mtime_s "$claim_root/$1/holder.info" "$claim_root/$1")
    if [ "$age" -gt "$LOCK_STALE_S" ]; then
        echo "LOCK $name: taking over the claim $1 held ${age}s by $(holder_text "$claim_root/$1/holder.info")"
        rm -rf "$claim_root/$1"
    fi
}

# Report the current holder at most once a minute while waiting. The caller
# brings the first-round line, because that line is the only place the two
# lock kinds differ: the claim wait line names the lock root (which is what
# shows up when two batches silently took two different locks), the
# write-directory wait line names the directory it wants, and the repeat
# lines read the same for both. The throttle itself lives here and nowhere
# else, so a wait can report at one rate whatever lock it is waiting on.
wait_round() { # <waited-seconds> <first-round-line> <holder-description>
    if [ "$1" -eq 0 ]; then
        echo "$2"
    elif [ $(( $1 % LOCK_REPORT_EVERY_S )) -eq 0 ]; then
        echo "LOCK $name: still waiting ${1}s ($3)"
    fi
}

# Take one directory lock per directory the command names for itself, so two
# legs writing one tree queue while legs writing different trees keep running
# side by side. Every leg asks, with no mode to opt in: the machine lock no
# longer exists to keep a test leg out of a build leg's way, and what is left
# of that job is exactly the directory claim plus the named claims for
# conflicts no path can name (gate timing design D-10, D-11).
acquire_wdirs() {
    local wdirs pair wdir_path key desc waited
    wdirs=$(derive_write_dirs "$@")
    # A leg that names no writable directory anywhere (flag token or inline
    # NLANG_GATE_WRITE_DIR) and declares no claim cannot be checked for a
    # conflicting write target, and running it silently unlocked is how two
    # batches end up in one tree again. Warning rather than failing is what
    # keeps the directory-less legs that are legitimate today — the checker
    # legs read and print, they write nothing — running.
    if [ -z "$wdirs" ]; then
        if [ -z "$claims" ]; then
            echo "LOCK $name: no write directory derived from the command line, no directory lock taken"
        fi
        return 0
    fi
    # Ascending key order, not argv order: see wdir_keyed_pairs. The pair is
    # split by hand at the first tab instead of with IFS=$'\t', because read
    # treats a tab as IFS whitespace and strips a leading one: an empty key half
    # (a digest tool that printed no hash) would then land the path in $key,
    # leave $wdir_path empty, and have the claim below wait for a lock directory
    # that can never be created. The key half is hex-only, so the first tab is
    # never ambiguous (spec D-8).
    while IFS= read -r pair; do
        [ -n "$pair" ] || continue
        key=${pair%%$'\t'*}
        wdir_path=${pair#*$'\t'}
        if [ -z "$key" ]; then
            # The same degradation as an undervivable directory above (spec
            # D-7): the leg still runs, but it names the directory that is
            # getting no lock, so a silently unlocked tree stays impossible.
            echo "LOCK $name: no lock key for $wdir_path (the digest tool printed no hash), no directory lock taken"
            continue
        fi
        waited=0
        while :; do
            # Re-made every round: a temp cleaner can take either parent away
            # while we wait, and without this the claim below would fail
            # forever. The claim is a bare mkdir because mkdir -p also reports
            # success for a directory that already exists, and reporting that
            # failure is the whole mutual exclusion.
            mkdir -p "$lock_dir" "$wdir_dir"
            sweep_stale_wdir "$key" "$wdir_path"
            if mkdir "$wdir_dir/$key" 2>/dev/null; then
                wdir_holder "$wdir_dir/$key/holder.info" "$wdir_path"
                wdir_held+="$key"$'\n'
                echo "LOCK $name: write-directory lock acquired after ${waited}s ($wdir_path)"
                break
            fi
            # The same wording and the same age source as the claim lock's wait
            # line, so a holder read through either lock says the same thing.
            desc="write-directory lock held $(holder_age_or_mtime_s "$wdir_dir/$key/holder.info" "$wdir_dir/$key")s by $(holder_text "$wdir_dir/$key/holder.info")"
            wait_round "$waited" "LOCK $name: waiting for the write-directory lock on $wdir_path ($desc)" "$desc"
            sleep "$LOCK_POLL_S"
            waited=$(( waited + LOCK_POLL_S ))
        done
    done <<< "$(printf '%s\n' "$wdirs" | wdir_keyed_pairs)"
}

# Take one named-resource lock per --claim, in ascending resource-name
# order and before the directory locks, so the acquisition order is the
# fixed claims → directories the header comment pins: with one total order
# across the two kinds, no two gates can each hold one lock and wait on the
# other (the same sufficient condition wdir_keyed_pairs states for
# directories, spec D-5). The resource cannot be derived from a path, so
# the caller names it; everything else — atomic mkdir as the claim, the
# holder file, the age-based takeover, the wait reporting — is the
# machinery the directory lock already provides.
acquire_claims() {
    local claim claim_dir desc waited
    [ -n "$claims" ] || return 0
    while IFS= read -r claim; do
        [ -n "$claim" ] || continue
        claim_dir="$claim_root/$claim"
        waited=0
        while :; do
            # Re-made every round for the same reason the other locks
            # re-make their parents: a temp cleaner can take the root away
            # while we wait, and then the mkdir below would fail forever.
            mkdir -p "$lock_dir" "$claim_root"
            sweep_stale_claim "$claim"
            if mkdir "$claim_dir" 2>/dev/null; then
                write_holder "$claim_dir/holder.info"
                claim_held+="$claim"$'\n'
                echo "LOCK $name: claim $claim acquired after ${waited}s"
                break
            fi
            desc="claim $claim held $(holder_age_or_mtime_s "$claim_dir/holder.info" "$claim_dir")s by $(holder_text "$claim_dir/holder.info")"
            wait_round "$waited" "LOCK $name: waiting for the claim $claim in $lock_dir ($desc)" "$desc"
            sleep "$LOCK_POLL_S"
            waited=$(( waited + LOCK_POLL_S ))
        done
    done < <(printf '%s\n' $claims | LC_ALL=C sort)
}

release() {
    # Releasing runs in the reverse of the order things were taken
    # (claims → directory locks): the directory locks go first and the
    # claims after them.
    release_wdirs
    release_claims
}

# Claim names in the reverse of the order they were taken, the same rule
# release_wdirs follows for keys. Only the holder and its flag directory go
# away; claim/ and the lock root stay.
release_claims() {
    local claim
    while IFS= read -r claim; do
        [ -n "$claim" ] || continue
        rm -f "$claim_root/$claim/holder.info"
        rmdir "$claim_root/$claim" 2>/dev/null || true
        echo "LOCK $name: claim $claim released"
    done < <(printf '%s' "$claim_held" | tac)
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
# Armed before the claims and the directory locks are taken, so a signal
# arriving while one is being waited on releases whatever is already claimed
# instead of leaving it for the next gate to age out.
trap release EXIT
acquire_claims
acquire_wdirs "$@"

# The coordination namespace belongs to this gate instance (R-100): a
# wrapped command that nests another gate must derive its claims from
# its own argv, and no production consumer reads the forwarded names.
# Tuning values were already captured into locals above; only the
# child's view goes blind.
strip_env=()
while IFS= read -r strip_name; do
    strip_env+=(-u "$strip_name")
done < <(compgen -e | grep '^NLANG_GATE_' || true)
env ${strip_env[@]+"${strip_env[@]}"} "$@" >"$stem.out" 2>"$stem.err"
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
