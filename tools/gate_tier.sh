#!/usr/bin/env bash
# gate_tier.sh -- assemble the tiered gate batches through gate.sh
# (docs/dev/gate_tiering_design.md).
#
#   gate_tier.sh T0 --master-tree DIR (--git BASE | --paths p...) [--docs]
#   gate_tier.sh T1 --master-tree DIR --asan-tree DIR --ide-tree DIR [--docs]
#   gate_tier.sh T2 --master-tree DIR --asan-tree DIR --ide-tree DIR
#
# T0  (per iteration, and the close-out of an ordinary task, minutes):
#     impact-mapped Debug unit families + matching e2e subset; any
#     unmapped changed path falls back to the FULL Debug unit suite
#     (never fewer tests).
# T1  (phase dock, ~1-1.5h; also the close-out of a task whose change
#     touches a memory-risk family — concurrency, containers,
#     GC/exceptions/serialization): full Debug units + full Debug e2e (cap
#     120x3, calibrated against stdlib_regex_size's 244s Debug reality)
#     + IDE tree + ASan units + ASan memory-risk e2e subset. The subset
#     is DERIVED at run time from tools/gate_tiers.txt (the stale-copy
#     lesson) — never stored.
# T2  (version close, release-time only): T1 legs + full ASan e2e +
#     Release build + perf class (the T2-perf leg runs the ON-registered
#     entries, its restore leg closes the cache window unconditionally)
#     + Release e2e + docs five legs + the carrier pairing guard.
#
# Every leg goes through tools/gate.sh (machine lock, verdict lines
# "GATE <leg>: PASS|FAIL"); all legs run regardless of earlier reds —
# any red voids the batch and forces a full re-sweep. --dry-run prints
# the planned legs without executing (assembly self-check).
#
# The exit code is 0 only when every leg printed a PASS verdict and the
# assembly itself worked; a red leg, a leg that never reached gate.sh
# (no verdict line) and a failed subset derivation all make it non-zero,
# so a caller scripting the batch cannot mistake a red batch for a green
# one (gate_selftest.sh case A10). NLANG_GATE_TIER_WORK redirects the log
# and scratch directory (default temp/tier) for a caller that must not
# collide with a batch already running.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PY="D:/dev/miniconda3/envs/py313/python.exe"
WORK="${NLANG_GATE_TIER_WORK:-$REPO/temp/tier}"
# Absolutized here: mkdocs resolves --site-dir against the config file's own
# directory, so a relative override had the build legs write under docs/ while
# the check legs walked the caller's directory and reported zero pages for a
# batch whose content was fine (gate_selftest.sh case A12).
case "$WORK" in
  /*|[A-Za-z]:*) ;;
  *) WORK="$PWD/$WORK" ;;
esac
LOGS="$WORK/gates"
DOCS_CONFIG_EN="$REPO/docs/mkdocs.en.yml"
DOCS_CONFIG_ZH="$REPO/docs/mkdocs.zh.yml"

TIER=""
MASTER_TREE=""
ASAN_TREE=""
IDE_TREE=""
GIT_BASE=""
DOCS=0
DRY=0
CHANGED_PATHS=()
LEG_NAMES=()
# The round's perf entry set ('|'-joined), computed once by perf_names()
# at the head of t0()/t1_legs(); ctest_leg unions it into every leg's -E
# (D-6). Declared here so set -u never trips before those heads run.
PERF_E=""

# Usage is the leading comment block: printing it by line count would need a
# number to keep in step with the header, so the block ends where the comments
# end.
usage() { awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    T0|T1|T2) TIER="$1" ;;
    --master-tree) MASTER_TREE="$2"; shift ;;
    --asan-tree) ASAN_TREE="$2"; shift ;;
    --ide-tree) IDE_TREE="$2"; shift ;;
    --git) GIT_BASE="$2"; shift ;;
    --paths) shift; while [ $# -gt 0 ] && [[ "$1" != --* ]]; do
               CHANGED_PATHS+=("$1"); shift; done; continue ;;
    --docs) DOCS=1 ;;
    --dry-run) DRY=1 ;;
    *) usage ;;
  esac
  shift
done

[ -z "$TIER" ] && usage
[ -z "$MASTER_TREE" ] && usage
# Trees are absolutized at parse time exactly like WORK above: the e2e
# legs run the suite runner from its own directory (tests/e2e), and a
# relative tree handed to it resolves its tool executables against THAT
# directory, not against this script's -- the calibration probe then
# asked for tests/e2e/build/.../ncc.exe and the leg read rc=2 "are the
# tools built?". cygpath spells the result the way both bash (cd) and
# the native interpreter accept; a host without cygpath keeps POSIX.
CYGPATH_BIN=$(command -v cygpath || true)
abs_tree() { # abs_tree <path> -> absolute Windows-style, forward slashes
  local p="$1"
  case "$p" in
    /*) ;;
    [A-Za-z]:*) printf '%s\n' "$p"; return ;;
    *) p="$PWD/$p" ;;
  esac
  if [ -n "$CYGPATH_BIN" ]; then
    printf '%s\n' "$("$CYGPATH_BIN" -w "$p" | tr '\\' '/')"
  else
    printf '%s\n' "$p"
  fi
}
MASTER_TREE=$(abs_tree "$MASTER_TREE")
[ -n "$ASAN_TREE" ] && ASAN_TREE=$(abs_tree "$ASAN_TREE")
[ -n "$IDE_TREE" ] && IDE_TREE=$(abs_tree "$IDE_TREE")
mkdir -p "$LOGS" "$WORK"
# The carrier guard (guard eleven) pairs the reference table against the
# leg commands themselves, so every leg of this batch -- dry runs
# included, that is where the assembly pins live -- records its full
# command here (name<TAB>command). Truncated per batch: batches run
# sequentially on this work directory, and a stale line from an earlier
# tier would pair against rows the current batch never carried.
: > "$LOGS/leg_commands.tsv"

leg() { # leg <name> [--gate-env NAME=VAL]... [--claim <resource>]... <command...>
  # The two leading declaration forms are leg-level metadata and must
  # precede the command. Placement discipline matters: never `export` --
  # an exported NLANG_GATE_WRITE_DIR would re-point every later gate.sh
  # run on this machine -- and never a call-site `VAR=v leg ...` prefix,
  # which works but hides the variable from the recorded line: the
  # carrier guard and the assembly pins read the declaration where it
  # lands, and gate.sh itself sees no command environment, only its own.
  # "$*" is captured before the split so leg_commands.tsv and the
  # --dry-run listing record every declaration exactly as written.
  # --claim becomes one --claim argument of gate.sh (the named-resource
  # lock serializes this whole leg against the other legs holding the
  # same resource, gate timing design D-11). --gate-env pairs are put in
  # front of the gate.sh call with `env`, so the variable lands in
  # gate.sh's own process environment -- derive_write_dirs reads
  # NLANG_GATE_WRITE_DIR there, and a declaration that only reached the
  # wrapped command's environment would lock nothing. Today every leg
  # still holds the machine lock, so the write-dir declaration changes
  # no scheduling yet; it is what the Task 12 machine-lock removal
  # builds on, and it already gives the dry-run listing and the carrier
  # guard the tree each leg writes.
  local name="$1"; shift
  local declared="$*"
  local claim_args=() env_pairs=() rest=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --claim) claim_args+=("$1" "$2"); shift 2 ;;
      --gate-env) env_pairs+=("$2"); shift 2 ;;
      *) rest+=("$1"); shift ;;
    esac
  done
  LEG_NAMES+=("$name")
  printf '%s\t%s\n' "$name" "$declared" >> "$LOGS/leg_commands.tsv"
  if [ "$DRY" = 1 ]; then
    echo "LEG $name: $declared"
    return 0
  fi
  env "${env_pairs[@]}" bash "$REPO/tools/gate.sh" \
      "${claim_args[@]}" "$name" "$LOGS/$name" "${rest[@]}" \
      > "$LOGS/$name.gate" 2>&1
  echo "leg $name rc=$?"
}

# Machine scale per leg (D-4, guard four): calibrated fresh, in the load
# form the leg itself will run, right before the leg is assembled. The
# value reaches the leg only through the command-prefix environment -- a
# ctest ENVIRONMENT property would override the inherited value for the
# very entries the leg measures, so nothing bakes it at configure time.
# An already-set NLANG_GATE_MACHINE_SCALE wins verbatim without sampling:
# the stub smoke pins its own number, and a caller that must not re-sample
# (same leg, same number, D-5) uses the same door. rc!=0 and the
# calibration output on stderr mean the leg must not run uncalibrated.
# The sampling round itself runs before the leg's gate.sh entry, so it is
# not machine-locked: two test batches overlapping in that window see
# each other's contention (D-4 keeps that case in its contention
# caveat -- a machine at the band edge fails calibration, not silently).
leg_scale() { # leg_scale <config-key> <source-flag> <path>
  local config="$1" flag="$2" path="$3" out rc scale
  if [ -n "${NLANG_GATE_MACHINE_SCALE:-}" ]; then
    printf '%s\n' "$NLANG_GATE_MACHINE_SCALE"
    return 0
  fi
  [ "$DRY" = 1 ] && { printf '<calibrated-at-run-time>'; return 0; }
  out=$("$PY" "$REPO/tools/gate_time_check.py" calibrate "$config" \
        "$flag" "$path" 2>&1)
  rc=$?
  scale=$(printf '%s\n' "$out" | \
          awk '/^SCALE /{for(i=1;i<=NF;i++) if($i ~ /^scale=/){sub(/^scale=/,"",$i); print $i; exit}}')
  if [ $rc -ne 0 ] || [ -z "$scale" ]; then
    # stderr, not stdout: the caller captures stdout for the scale value,
    # and the calibration report is the failure's whole content (guard
    # four findings with the candidate rows).
    printf '%s\n' "$out" >&2
    return 1
  fi
  printf '%s\n' "$scale"
}

# A leg whose calibration failed still gets its gate.sh entry (false), so
# the batch conjunction shows the red verdict line instead of a leg that
# silently ran with an unknown scale; the calibration report went to the
# batch console above it.
leg_uncalibrated() { # leg_uncalibrated <name>
  leg "$1" false
}

# Per-entry watchdog split (guard three's kill side, design D-2): an
# entry that holds a reference row carries a configure-time derived
# TIMEOUT property (the derived-caps block in tests/CMakeLists.txt),
# and that property takes precedence over --timeout, so the flag owns
# exactly the row-less entries -- the ones the table does not price.
# The value equals ctest's own built-in default and sits far above the
# worst legitimate run, so it only ever catches a genuine hang while
# keeping the kill point explicit in the leg command.
CTEST_DEFAULT_TIMEOUT_S=1500

# The committed reference table: the single source for the schedule-axis
# exclusions (the slow set, derived from the baseline rows' ref_s --
# D-16/D-17), the calibration sentinels and the post-hoc checkers.
# Every tier batch reads this same path, so no leg ever judges against
# a stale copy.
REF_TABLE="$REPO/tests/gate_time_reference.txt"

# The config's slow set as one -E regex (D-7): print-slow owns the
# membership AND the -R backfill subtraction (ctest -E beats -R, F-12),
# so the filter exists in exactly one place -- the mapping regex goes to
# it verbatim, never as a name list. A red print-slow must fail the
# assembly, not silently run the slow entries: the caller checks rc.
slow_regex() { # slow_regex <config-key> [<mapped -R regex>]
  local out
  if [ $# -gt 1 ] && [ -n "$2" ]; then
    out=$("$PY" -I "$REPO/tools/gate_time_check.py" print-slow "$1" \
          --table "$REF_TABLE" --keep-matched "$2") || return 1
  else
    out=$("$PY" -I "$REPO/tools/gate_time_check.py" print-slow "$1" \
          --table "$REF_TABLE") || return 1
  fi
  printf '%s\n' "$out" | grep -v '^GATE '
}

# The perf class for the round (D-6): print-perf-names computes the
# ON/OFF registration difference once per round, in its own scratch
# directory (the sweep tree's cache stays untouched; only its seed-cache
# shape is read). The value feeds every ctest leg's -E union, the
# T2-perf leg's -R and the carrier checker; a red print-perf-names fails
# the assembly like a red print-slow (the caller turns it into the
# T-perf-names red leg). The file it writes is read back by check-carrier
# after the ON-cache window has closed and the tree enumerates none of
# these entries any more.
perf_names() {
  local out
  out=$("$PY" -I "$REPO/tools/gate_time_check.py" print-perf-names \
        --seed-tree "$MASTER_TREE" --scratch "$WORK/perf_scratch") \
        || return 1
  printf '%s\n' "$out" | grep -v '^GATE ' | tr -d '\r\n' \
      > "$WORK/perf_names.txt"
  cat "$WORK/perf_names.txt"
}

# Join two '|'-separated regex sets into one (deduped, byte-stable
# under LC_ALL=C for the pins; empty halves drop): the perf union and a
# leg's own -E must never spell one entry twice.
join_regex() { # join_regex <a> <b>
  printf '%s|%s' "$1" "$2" | tr '|' '\n' | grep -v '^$' | \
      LC_ALL=C sort -u | paste -sd'|' -
}

# A leg's output lands in a log file, not on a terminal, so every timed
# leg's environment pins one I/O encoding for its Python processes: on a
# machine whose ANSI code page cannot spell the checker's Chinese
# advisory wording, an uncaught UnicodeEncodeError would otherwise turn
# the budget leg into a false red. The pin rides the leg command's
# environment only -- the interactive console is never injected, so the
# operator's terminal keeps its own code page.
PY_IO_ENCODING="PYTHONIOENCODING=utf-8"

ctest_leg() { # ctest_leg <name> <tree> <config-key> [-R <regex>] [-E <regex>]
  local name="$1" tree="$2" config="$3"; shift 3
  local scale raw_r="" regex="" ereg="" junit="$LOGS/$name.xml"
  [ $# -gt 0 ] && [ -n "$1" ] && raw_r="$1" && regex=" -R '$1'"
  # The perf set rides EVERY ctest leg's -E, mapped or not (D-6): the
  # ON-only entries sit in no tree enumeration, so a leg that skipped
  # them would run the perf class inside its budget-priced round. The
  # join keeps the F-12 subtraction print-slow already applied to the
  # leg's own -E intact.
  local perf_e="$PERF_E"
  [ $# -gt 1 ] && [ -n "$2" ] && perf_e=$(join_regex "$PERF_E" "$2")
  [ -n "$perf_e" ] && ereg=" -E '$perf_e'"
  scale=$(leg_scale "$config" --tree "$tree") || { leg_uncalibrated "$name"; return; }
  # The work directory is reused across batches, and ctest only writes the
  # junit once it has started: clearing the old file first means the
  # checker either judges this round's timings or reports a missing file
  # (rc=1), never last round's numbers.
  rm -f "$junit"
  # --output-on-failure (R-59): the junit hands the checkers the
  # timings and the per-entry verdict but never the failing test's own
  # output, so without this flag a red ctest leg forces a manual
  # re-run just to see why; a green leg prints nothing extra.
  #
  # The nide-gui claim (D-11) is decided at assembly time without any
  # live enumeration, from two facts the batch already knows. First, the
  # window-launching entries the roster names register only in an IDE-
  # built tree -- the same CMakeCache shape predicate guard eleven uses
  # to pair a tree with ctest-IDE -- so a plain or instrumented tree can
  # never run one and takes no claim. Second, the leg must be able to
  # actually reach a roster entry with its own selectors: ctest -E beats
  # -R (F-12), so an entry inside the leg's -E is out no matter what -R
  # says, and a given -R admits an entry only if it matches. grep -E
  # stands in for ctest's matcher here: both sides only ever see this
  # repo's alternation/anchor patterns, and the decision errs safe -- a
  # spurious match can only add serialization, never remove it from a
  # leg that does sweep a windowed entry. The single roster source is
  # the claim line in tools/gate_tiers.txt.
  local claim_args=() roster entry
  # tr -d '\r' first: an anchored pattern must not fork on a CRLF cache
  # copy (a configure on this host writes LF; a moved cache may not).
  if tr -d '\r' < "$tree/CMakeCache.txt" 2>/dev/null | \
     grep -q '^NLANG_BUILD_IDE:BOOL=ON$'; then
    roster=$(sed -n 's/^claim nide-gui //p' "$REPO/tools/gate_tiers.txt")
    for entry in $roster; do
      if [ -n "$raw_r" ] && ! printf '%s\n' "$entry" | grep -qE -- "$raw_r"; then
        continue
      fi
      if [ -n "$perf_e" ] && printf '%s\n' "$entry" | grep -qE -- "$perf_e"; then
        continue
      fi
      claim_args=(--claim nide-gui)
      break
    done
  fi
  leg "$name" --gate-env "NLANG_GATE_WRITE_DIR=$tree" "${claim_args[@]}" \
    env "NLANG_GATE_MACHINE_SCALE=$scale" "$PY_IO_ENCODING" \
    bash -c \
    "cd '$tree' && ctest -C Debug --output-on-failure$regex$ereg --output-junit '$junit' --timeout $CTEST_DEFAULT_TIMEOUT_S"
  # Post-hoc budget judging (guard three): ctest has no inline budget
  # knob, so the checker reads the leg's junit after the fact. Its own
  # gate.sh entry, never spliced into the leg's command string -- a
  # fused word fuses the two verdicts (gate_selftest.sh's lesson). It
  # reuses this same leg's calibrated scalar (D-5: same leg, same
  # number), and --parallel/--repeat/--work-factor state the leg's run
  # form so the checker can refuse a form mismatch (guard ten).
  leg "$name-budget" env "$PY_IO_ENCODING" \
    "$PY" "$REPO/tools/gate_time_check.py" check-budget \
    --junit "$junit" --config "$config" --parallel 1 --repeat none \
    --work-factor 1 --scalar "$scale"
  # Registered-cap judging (guard five, R-30): the same post-hoc shape
  # as the budget checker -- its own gate.sh entry, its own verdict
  # line, never spliced into the leg's command string -- and it judges
  # THIS leg's own build tree: the --tree value is spelled out on the
  # command line, never inferred from a binary path. One sampling per
  # decision tree per round (the --show-only enumeration costs seconds).
  leg "$name-timeouts" env "$PY_IO_ENCODING" \
    "$PY" "$REPO/tools/gate_time_check.py" check-timeouts \
    --tree "$tree" --table "$REF_TABLE"
}

e2e_leg() { # e2e_leg <name> <tree> <config> <ref-key> [manifest] [timeout]
  local name="$1" tree="$2" config="$3" refkey="$4" manifest="$5" timeout="$6"
  timeout=${timeout:-120}
  manifest=${manifest:-$REPO/tests/e2e/manifest.txt}
  local scale env_args=(env)
  scale=$(leg_scale "$refkey" --tools "$tree") || { leg_uncalibrated "$name"; return; }
  env_args+=("NLANG_GATE_MACHINE_SCALE=$scale" "$PY_IO_ENCODING")
  if [ -n "${5:-}" ]; then
    env_args+=("NLANG_E2E_MANIFEST=$manifest")
  fi
  # The table rows the runner judges against are this leg's own column
  # (D-5: same leg, same row), so the reference key minus its e2e-
  # prefix goes to --config: a leg never leans on the runner's tool-path
  # guess, which would misprice the ASan sweep (its rows sit in e2e-ASan
  # while the instrumented binaries sit in /Debug/) and silence the
  # guess line that only a genuine guess deserves. The <config>
  # parameter keeps naming the tool binary's build config, exactly the
  # split the two differ on.
  local judging="${refkey#e2e-}"
  # The e2e runner works from tests/e2e and writes its per-test scratch
  # under tests/temp (gate_selftest.sh's scratch lesson), a repo-level
  # directory shared by every sweep tree -- so the declared write dir is
  # the repo's own temp, absolutized like the trees (the runner may be
  # reached from either path form, and the lock key must not fork on it).
  leg "$name" --gate-env \
    "NLANG_GATE_WRITE_DIR=$(abs_tree "$REPO/tests/temp")" \
    "${env_args[@]}" \
    "$PY" "$REPO/tests/e2e/run_e2e_tests.py" \
    "$tree/src/tools/ncc/$config/ncc.exe" \
    "$tree/src/tools/nvm/$config/nvm.exe" \
    "$tree/src/tools/ndb/$config/ndb.exe" \
    "--timeout=$timeout" "--config=$judging"
}

#The ASan subset manifest, derived fresh from gate_tiers.txt every run
#(subsets are derived artifacts — never stored).
derive_asan_subset() {
  local selector
  selector=$(sed -n 's/^tier T1-asan-subset selector //p' \
      "$REPO/tools/gate_tiers.txt")
  [ -n "$selector" ] || { echo "T1-asan-subset selector missing" >&2; return 1; }
  grep -E "$selector" "$REPO/tests/e2e/manifest.txt" \
      > "$WORK/asan_subset_derived.txt"
  if [ ! -s "$WORK/asan_subset_derived.txt" ]; then
    #A selector matching zero manifest rows is a mapping/table
    #inconsistency; running the leg would be vacuously green.
    echo "T1-asan-subset selector '$selector' matched no manifest rows" >&2
    return 2
  fi
}

docs_legs() {
  [ "$DOCS" = 1 ] || return 0
  local ncc="$MASTER_TREE/src/tools/ncc/Release/ncc.exe"
  local nvm="$MASTER_TREE/src/tools/nvm/Release/nvm.exe"
  leg "T-docs-pytest" env PYTHONPATH="$REPO/tools/nlang-docs/src" \
    "NLANG_NCC=$ncc" "NLANG_NVM=$nvm" \
    "$PY" -m pytest "$REPO/tools/nlang-docs/tests" -q
  #Both builds precede both checks: the checker walks the cross-language
  #links (en pages point at ../../zh/index.html and the reverse), so a
  #language checked before its sibling site exists reports that sibling's
  #home page as a missing target. The per-language build-then-check order
  #only ever passed because an earlier run had left the sibling site on
  #disk.
  for lang in en zh; do
    local config="$REPO/docs/mkdocs.$lang.yml"
    leg "T-docs-build-$lang" env PYTHONPATH="$REPO/tools/nlang-docs/src" \
      "$PY" -m nlang_docs build --config "$config" \
      --site-dir "$WORK/docs-site/$lang" --ncc "$ncc" --nvm "$nvm" \
      --defer-site-audit
  done
  for lang in en zh; do
    local config="$REPO/docs/mkdocs.$lang.yml"
    leg "T-docs-check-$lang" env PYTHONPATH="$REPO/tools/nlang-docs/src" \
      "$PY" -m nlang_docs check --site-dir "$WORK/docs-site/$lang" \
      --config "$config"
  done
}

t1_legs() { # t1_legs <slow-e on|off>
  # The perf set for the whole round is computed HERE, once (both the
  # t1 direct form and the t2 reuse form get it; the carrier file is
  # written in the same call). A red print-perf-names must not silently
  # run the perf entries inside the budget legs: the batch records the
  # failure as its own red leg and aborts the assembly.
  PERF_E=$(perf_names) || { leg T-perf-names false; return 2; }
  # The shared legs (D-7): the T1 direct form (t1() passes on) excludes
  # the slow set -- print-slow derives membership from each entry's
  # baseline row ref_s against SLOW_MIN_REF_S (D-16/D-17) and projects
  # the one repo-wide roster onto every config's -E, and the T1 legs
  # carry no -R, so there is no mapping hit to subtract. The T2
  # close-out reuses these same legs WITHOUT the -E (t2() passes off):
  # the close-out batch must actually run the slow members -- the D-7
  # carrier clause and guard eleven lean on exactly this. A red
  # print-slow aborts the assembly rather than silently sweeping the
  # slow entries.
  local slow_unit="" slow_ide="" slow_asan=""
  if [ "$1" = on ]; then
    slow_unit=$(slow_regex ctest-Debug) || return 2
    slow_ide=$(slow_regex ctest-IDE) || return 2
    slow_asan=$(slow_regex ctest-ASan) || return 2
  fi
  ctest_leg T1-debug-unit "$MASTER_TREE" ctest-Debug "" "$slow_unit"
  # T1's e2e legs are the full scan: the whole manifest, examples pass
  # included (the manifest default is spelled out for the pin), and the
  # slow e2e entries are excluded from T0 rounds only.
  e2e_leg T1-debug-e2e "$MASTER_TREE" Debug e2e-Debug \
      "$REPO/tests/e2e/manifest.txt" 120
  ctest_leg T1-ide-ctest "$IDE_TREE" ctest-IDE "" "$slow_ide"
  ctest_leg T1-asan-unit "$ASAN_TREE" ctest-ASan "" "$slow_asan"
  derive_asan_subset || return 2
  e2e_leg T1-asan-e2e-subset "$ASAN_TREE" Debug e2e-ASan \
      "$WORK/asan_subset_derived.txt" 180
}

t0() {
  # Same per-round perf set as t1_legs() (the T0 unit leg's -E carries
  # the union too; D-6 says no scheduled round runs the perf class).
  PERF_E=$(perf_names) || { leg T-perf-names false; return 2; }
  local impact_args=()
  if [ -n "$GIT_BASE" ]; then
    impact_args=($(git diff --name-only "$GIT_BASE" HEAD))
  else
    impact_args=("${CHANGED_PATHS[@]}")
  fi
  [ ${#impact_args[@]} -gt 0 ] || { echo "T0: no changed paths"; return 2; }
  local map rc unit_regex e2e_regex keep_regex=""
  map=$("$PY" -I "$REPO/tools/test_impact.py" \
      "$REPO/tools/test_families.txt" "${impact_args[@]}")
  rc=$?
  unit_regex=$(awk '$1=="unit"{print $2}' <<<"$map")
  e2e_regex=$(awk '$1=="e2e"{print $2}' <<<"$map")
  if [ $rc -eq 3 ]; then
    echo "T0: unmapped paths -> conservative full Debug units"
    unit_regex="."
  elif [ $rc -ne 0 ]; then
    echo "T0: impact mapping failed rc=$rc"; return 2
  else
    keep_regex="$unit_regex"
  fi
  if [ -n "$unit_regex" ]; then
    # Slow-set exclusion (D-7): print-slow owns the membership and the
    # F-12 backfill subtraction, but the -R selector and the regex fed
    # to it are two different values (R-53). Mapped round: the mapping
    # regex goes to print-slow verbatim, so an entry both mapped and
    # slow stays in the round through -R instead of vanishing under -E
    # (ctest -E beats -R). Unmapped conservative round: the selector is
    # "." -- everything -- and nothing needs mapping back, so NO
    # --keep-matched goes out: "." re.search-hits every slow entry and
    # would empty the subtraction, running the slow entries inside the
    # fast round (the 466 s timing gate with its load-induced false
    # red). Without the flag print-slow keeps the whole slow set, the
    # same exclusion the full-scan T1 unit legs carry.
    local slow_unit
    slow_unit=$(slow_regex ctest-Debug "$keep_regex") \
        || { echo "T0: print-slow failed"; return 2; }
    ctest_leg T0-unit "$MASTER_TREE" ctest-Debug "$unit_regex" "$slow_unit"
  fi
  if [ -n "$e2e_regex" ]; then
    # This round's e2e lines come from derive-e2e-subset (R-64's set
    # formula: mapping hits unioned with the lines carrying their own
    # explicit non-default row under this config while sitting outside
    # the one global slow set, manifest order, deduped) -- the
    # selection lives in that one place, so guard six can judge the
    # very formula the wiring runs, never a grep re-implementation of
    # it.
    local subset
    subset=$("$PY" -I "$REPO/tools/gate_time_check.py" derive-e2e-subset \
        e2e-Debug --table "$REF_TABLE" \
        --manifest "$REPO/tests/e2e/manifest.txt" \
        --mapping "$e2e_regex") \
        || { echo "T0: derive-e2e-subset failed"; return 2; }
    printf '%s\n' "$subset" | grep -v '^GATE ' > "$WORK/t0_subset.txt"
    if [ ! -s "$WORK/t0_subset.txt" ]; then
      #A nonempty selector matching zero manifest rows is a mapping/
      #table inconsistency; running the leg would be vacuously green.
      echo "T0: e2e selector '$e2e_regex' selected no manifest rows"
      return 2
    fi
    e2e_leg T0-e2e "$MASTER_TREE" Debug e2e-Debug "$WORK/t0_subset.txt" 120
  fi
  [ ${#LEG_NAMES[@]} -gt 0 ] || { echo "T0: nothing to run"; return 0; }
}

t1() { t1_legs on; docs_legs; }

t2() {
  DOCS=1  #version close always includes the docs five legs
  t1_legs off  # the close-out sweep runs the slow entries itself (D-7)
  e2e_leg T2-asan-e2e-full "$ASAN_TREE" Debug e2e-ASan \
      "$REPO/tests/e2e/manifest.txt" 180
  leg T2-release-build bash -c \
      "cmake --build '$MASTER_TREE' --config Release"
  # The perf class (D-6): one leg opens the ON-registration window on
  # the sweep tree once its Release binaries exist (the flag gates test
  # registration only, so nothing recompiles), runs the differential
  # under -C Release -- the only key the table prices these entries --
  # and prices them against that same key (the budget reuses this
  # leg's own scalar, D-5). The leg carries its names in -R and no -E
  # of its own: an -E over exactly the perf names would cancel the -R
  # outright (F-12). The restore leg is unconditional -- a red perf run
  # must never leave the tree registered differently from what every
  # later batch and checker reads -- and the batch runs the remaining
  # legs regardless, so the window closes whatever happened above it.
  # The leg runs with --output-on-failure (R-59), the same failure-
  # output guarantee every ctest_leg call gives.
  if [ -n "$PERF_E" ]; then
    local scale
    scale=$(leg_scale ctest-Release --tree "$MASTER_TREE") \
        || leg_uncalibrated T2-perf
    if [ -n "$scale" ]; then
      rm -f "$LOGS/T2-perf.xml"
      leg T2-perf env "NLANG_GATE_MACHINE_SCALE=$scale" "$PY_IO_ENCODING" \
        bash -c \
        "cmake -S '$REPO' -B '$MASTER_TREE' -DNLANG_PERF_TESTS=ON && cd '$MASTER_TREE' && ctest -C Release --output-on-failure -R '^($PERF_E)$' --output-junit '$LOGS/T2-perf.xml' --timeout $CTEST_DEFAULT_TIMEOUT_S"
      leg T2-perf-budget env "$PY_IO_ENCODING" \
        "$PY" "$REPO/tools/gate_time_check.py" check-budget \
        --junit "$LOGS/T2-perf.xml" --config ctest-Release --parallel 1 \
        --repeat none --work-factor 1 --scalar "$scale"
      leg T2-perf-timeouts env "$PY_IO_ENCODING" \
        "$PY" "$REPO/tools/gate_time_check.py" check-timeouts \
        --tree "$MASTER_TREE" --table "$REF_TABLE"
    fi
    leg T2-perf-restore bash -c \
        "cmake -S '$REPO' -B '$MASTER_TREE' -DNLANG_PERF_TESTS=OFF"
  fi
  e2e_leg T2-release-e2e "$MASTER_TREE" Release e2e-Release "" 120
  docs_legs
  # Guard eleven last, once the assembly is complete: it pairs every
  # slow-member row and every ASan/Release row against the commands
  # this batch recorded in leg_commands.tsv, so every leg it needs to
  # see already has its line, and its verdict rides the batch
  # conjunction like any other leg's.
  leg T2-carrier env "$PY_IO_ENCODING" \
    "$PY" "$REPO/tools/gate_time_check.py" check-carrier \
    --table "$REF_TABLE" --leg-commands "$LOGS/leg_commands.tsv" \
    --perf-names "$WORK/perf_names.txt"
}

case "$TIER" in
  T0) t0 ;;
  T1) t1 ;;
  T2) t2 ;;
esac
batch_rc=$?

echo "===== $TIER verdicts ====="
verdict_rc=0
if [ "$DRY" = 1 ]; then
  # A dry run checks the assembly, not the tree: the legs were listed and never
  # run, so a missing verdict is what it should look like, not a red batch.
  for name in ${LEG_NAMES[@]+"${LEG_NAMES[@]}"}; do
    echo "GATE $name: NO-VERDICT (dry run)"
  done
else
  # The verdict lines are the batch's result: a leg that is red and a leg that
  # never reached gate.sh (no line of its own) both keep the batch non-zero.
  for name in ${LEG_NAMES[@]+"${LEG_NAMES[@]}"}; do
    line=$(grep -h "GATE $name" "$LOGS/$name.gate" 2>/dev/null || \
           echo "GATE $name: NO-VERDICT (the leg printed no verdict)")
    echo "$line"
    [[ "$line" == *FAIL* || "$line" == *NO-VERDICT* ]] && verdict_rc=1
  done
  # An assembly failure after some legs ran (a missing subset selector) leaves
  # every leg green and must still turn the batch red.
  [ "$batch_rc" -ne 0 ] && verdict_rc=1
fi
exit "$verdict_rc"
