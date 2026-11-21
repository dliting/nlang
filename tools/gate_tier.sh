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
#     Release build + Release e2e + docs five legs.
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
mkdir -p "$LOGS" "$WORK"

leg() { # leg <name> <command...>
  local name="$1"; shift
  LEG_NAMES+=("$name")
  if [ "$DRY" = 1 ]; then
    echo "LEG $name: $*"
    return 0
  fi
  bash "$REPO/tools/gate.sh" "$name" "$LOGS/$name" "$@" > "$LOGS/$name.gate" 2>&1
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

# A leg's output lands in a log file, not on a terminal, so every timed
# leg's environment pins one I/O encoding for its Python processes: on a
# machine whose ANSI code page cannot spell the checker's Chinese
# advisory wording, an uncaught UnicodeEncodeError would otherwise turn
# the budget leg into a false red. The pin rides the leg command's
# environment only -- the interactive console is never injected, so the
# operator's terminal keeps its own code page.
PY_IO_ENCODING="PYTHONIOENCODING=utf-8"

ctest_leg() { # ctest_leg <name> <tree> <config-key> [-R <regex>]
  local name="$1" tree="$2" config="$3"; shift 3
  local scale regex="" junit="$LOGS/$name.xml"
  [ $# -gt 0 ] && [ -n "$1" ] && regex=" -R '$1'"
  scale=$(leg_scale "$config" --tree "$tree") || { leg_uncalibrated "$name"; return; }
  # The work directory is reused across batches, and ctest only writes the
  # junit once it has started: clearing the old file first means the
  # checker either judges this round's timings or reports a missing file
  # (rc=1), never last round's numbers.
  rm -f "$junit"
  leg "$name" env "NLANG_GATE_MACHINE_SCALE=$scale" "$PY_IO_ENCODING" \
    bash -c \
    "cd '$tree' && ctest -C Debug$regex --output-junit '$junit' --timeout $CTEST_DEFAULT_TIMEOUT_S"
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
  leg "$name" "${env_args[@]}" \
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

t1_legs() {
  ctest_leg T1-debug-unit "$MASTER_TREE" ctest-Debug
  e2e_leg T1-debug-e2e "$MASTER_TREE" Debug e2e-Debug "" 120
  ctest_leg T1-ide-ctest "$IDE_TREE" ctest-IDE
  ctest_leg T1-asan-unit "$ASAN_TREE" ctest-ASan
  derive_asan_subset || return 2
  e2e_leg T1-asan-e2e-subset "$ASAN_TREE" Debug e2e-ASan \
      "$WORK/asan_subset_derived.txt" 180
}

t0() {
  local impact_args=()
  if [ -n "$GIT_BASE" ]; then
    impact_args=($(git diff --name-only "$GIT_BASE" HEAD))
  else
    impact_args=("${CHANGED_PATHS[@]}")
  fi
  [ ${#impact_args[@]} -gt 0 ] || { echo "T0: no changed paths"; return 2; }
  local map rc unit_regex e2e_regex
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
  fi
  if [ -n "$unit_regex" ]; then
    ctest_leg T0-unit "$MASTER_TREE" ctest-Debug "$unit_regex"
  fi
  if [ -n "$e2e_regex" ]; then
    grep -E "$e2e_regex" "$REPO/tests/e2e/manifest.txt" \
        > "$WORK/t0_subset.txt"
    if [ ! -s "$WORK/t0_subset.txt" ]; then
      #A nonempty selector matching zero manifest rows is a mapping/
      #table inconsistency; running the leg would be vacuously green.
      echo "T0: e2e selector '$e2e_regex' matched no manifest rows"
      return 2
    fi
    e2e_leg T0-e2e "$MASTER_TREE" Debug e2e-Debug "$WORK/t0_subset.txt" 120
  fi
  [ ${#LEG_NAMES[@]} -gt 0 ] || { echo "T0: nothing to run"; return 0; }
}

t1() { t1_legs; docs_legs; }

t2() {
  DOCS=1  #version close always includes the docs five legs
  t1_legs
  e2e_leg T2-asan-e2e-full "$ASAN_TREE" Debug e2e-ASan \
      "$REPO/tests/e2e/manifest.txt" 180
  leg T2-release-build bash -c \
      "cmake --build '$MASTER_TREE' --config Release"
  e2e_leg T2-release-e2e "$MASTER_TREE" Release e2e-Release "" 120
  docs_legs
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
