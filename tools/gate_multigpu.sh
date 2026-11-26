#!/usr/bin/env bash
# gate_multigpu.sh — the two-device acceptance leg of the box's gate
# script family (sits beside gate_cuda_tree.sh; multi_gpu_design.md
# §8, multi_gpu_impl_plan.md §5).
#
# The two-device run is the first execution site of this line's
# cross-device paths on real hardware. On a machine with fewer than
# two CUDA devices the leg cannot train anything: it reports SKIP
# with the reason and exits 0 (the skip contract, plan §1 — the
# program's own exit-90 sentinel is the same rule at the program
# level).
#
# Usage: tools/gate_multigpu.sh <cuda-build-tree> [logfile-stem]
#   <cuda-build-tree>  a configured cuda tree (build-cuda on the
#                      acceptance box); the leg rebuilds it first so
#                      binaries never drift from the checkout
#   [logfile-stem]     capture prefix (default: temp/gate_multigpu)
#
# Steps (plan §5, acceptance as written there):
# 1. Fresh cuda build of the tree (incremental; a configure/build
#    failure fails the leg).
# 2. Device probe: a working cuda backend with deviceCount() >= 2,
#    else SKIP (explicit-cuda fail-fast rc included in the reason).
# 3. E2E double-run of the device subset
#    (tools/e2e_cuda_double_run.txt): both runs exit 0 and their
#    verdict lists are byte-identical — the corpus's run-to-run
#    contract, now carrying the cross-device transfer paths.
# 4. Single-GPU baseline re-run fresh in this script (mnist_mlp,
#    seeded): the comparison point steps 5-6 measure against, so
#    the leg never trusts a number recorded on another machine.
# 5. mnist_ddp twice: both exit 0, the epoch-loss sequences are
#    byte-identical between the runs, both accuracies sit at or
#    above the floor and within the baseline margin of the fresh
#    baseline, and the per-epoch loss curve stays within
#    DDP_LOSS_BAND of the baseline curve (a calibrated cap like the
#    branch's timeout caps, not a mathematical assertion).
# 6. Occupancy: the nvidia-smi sampler run across step 5 observed
#    GPU-Util > 0 on every device index (spec §8's both-GPUs-busy
#    check).
#
# Exit: 0 = PASS or SKIP, non-zero = FAIL. Read the verdict line,
# not the tails.

set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
tree="${1:?usage: gate_multigpu.sh <cuda-build-tree> [logfile-stem]}"
stem="${2:-temp/gate_multigpu}"
python="${PYTHON:-python3}"

# The loss-curve band: max |ddp epoch loss - baseline epoch loss|
# tolerated for one run of the same seeded configuration. The
# deviation sources are float reduction order (shard-means summed
# then halved vs the full-batch mean) and the extra multiply by
# 0.5f — noise of the same family as the 0.897-vs-0.896 accuracy
# wobble across machines. Calibrated 2026-11-26 on the 2×4090
# acceptance box: all 40 epochs of ddp vs the fresh in-script
# baseline matched inside the printed precision (< 1e-6, and the
# accuracies were identical at 0.896); the band sits three orders
# above the observed maximum and still two orders below the loss
# scales that matter here (initial loss 1.4). A calibrated cap,
# not a mathematical assertion. Setting the band to "CALIBRATE"
# makes the leg report the observed maximum without enforcing.
DDP_LOSS_BAND="0.001"
# Accuracy gates (plan §5): the acceptance floor and the maximum
# distance from the baseline this script re-measures.
ACC_FLOOR="0.89"
ACC_BASE_MARGIN="0.01"

cd "$root"
mkdir -p temp
# A stem with a directory prefix (the default temp/gate_multigpu is
# one) needs that directory to exist: a missing directory makes the
# first redirect fail, and the failure lands on whichever step runs
# first — misdiagnosed as "the build broke". One resolution up front
# also keeps the log and package paths absolute.
case "$stem" in
    /*) ;;
    *) stem="$root/$stem" ;;
esac
mkdir -p "$(dirname "$stem")"
tree_abs="$(cd "$tree" 2>/dev/null && pwd)" || {
    echo "GATE multigpu: FAIL (build tree not found: $tree)"
    exit 1
}
ncc="$tree_abs/src/tools/ncc/ncc"
nvm="$tree_abs/src/tools/nvm/nvm"
ndb="$tree_abs/src/tools/ndb/ndb"

fail () { echo "GATE multigpu: FAIL $*"; exit 1; }

# --- 1. fresh cuda build -------------------------------------------
cmake --build "$tree_abs" -j "$(nproc)" > "$stem.build.log" 2>&1 \
    || fail "build broke (see $stem.build.log)"
for tool in "$ncc" "$nvm" "$ndb"; do
    [ -f "$tool" ] || fail "missing $tool after build"
done

# --- 2. two-device probe --------------------------------------------
probe_src="$root/temp/_gate_multigpu_probe.n"
cat > "$probe_src" << 'EOF'
import io;
import tensor;

int main() {
    if (tensor.backend() != "cuda") return 1;
    int n = tensor.deviceCount();
    io.print("deviceCount = " + n);
    if (n < 2) return 2;
    return 0;
}
EOF
probe_ncu="$root/temp/_gate_multigpu_probe.ncu"
rm -f "$probe_ncu"
( cd temp && "$ncc" build _gate_multigpu_probe.n > /dev/null 2>&1 )
[ -f "$probe_ncu" ] || fail "probe did not compile"
probe_out="$( cd temp && NLANG_TENSOR_BACKEND=cuda "$nvm" \
    _gate_multigpu_probe.ncu 2>&1 )"
probe_rc=$?
if [ "$probe_rc" -ne 0 ]; then
    echo "GATE multigpu: SKIP (needs a cuda backend with >=2 devices: rc=$probe_rc: $probe_out)"
    exit 0
fi

# --- 3. e2e double-run over the device subset -----------------------
run_verdicts () {  # $1 = run suffix
    NLANG_E2E_TENSOR_BACKEND=cuda "$python" tests/e2e/run_e2e_tests.py \
        "$ncc" "$nvm" "$ndb" tools/e2e_cuda_double_run.txt \
        > "$stem.$1.log" 2>&1 || return 1
    grep -E '^(PASS|FAIL|SKIP)' "$stem.$1.log" > "$stem.$1.verdicts"
}
run_verdicts runA || { tail -n 5 "$stem.runA.log"; fail "roster run A nonzero"; }
run_verdicts runB || { tail -n 5 "$stem.runB.log"; fail "roster run B nonzero"; }
# The box is the maximally capable machine of the fleet: every entry
# of the device subset must really run there (a SKIP line would mean
# a silently untested entry — plan §1's honesty rule).
skipsA="$(grep -c '^SKIP' "$stem.runA.log")"
[ "$skipsA" -eq 0 ] || fail "roster run A skipped $skipsA entries on the reference box"
cmp -s "$stem.runA.verdicts" "$stem.runB.verdicts" \
    || fail "roster verdict lists diverged: $(diff "$stem.runA.verdicts" \
        "$stem.runB.verdicts" | head -n 5 | tr '\n' ' ')"

# --- helpers ----------------------------------------------------------
extract_losses () { grep -E '^epoch [0-9]+: loss ' "$1" | awk '{print $4}'; }
extract_acc ()    { grep -E '^test accuracy ' "$1" | awk '{print $3}'; }
# $1 = npkg path, $2 = log suffix: the stem was resolved absolute up
# front, so both paths survive the subshell's cd untouched.
train_run () {
    ( cd examples/mnist_mlp && NLANG_TENSOR_BACKEND=cuda "$nvm" \
        "$1" > "$stem.$2.log" 2>&1 )
}

# --- 4. the fresh single-GPU baseline --------------------------------
# The npkg names join the stem with underscores, not dots: a package
# derives its module name from the file stem, and a dotted stem is not
# a valid module name. The log files carry dots (they name no module).
"$ncc" build -p examples/mnist_mlp/mnist_mlp.nproj \
    -o "${stem}_mnist_mlp.npkg" > "$stem.mlpbuild.log" 2>&1 \
    || fail "baseline did not compile (see $stem.mlpbuild.log)"
train_run "${stem}_mnist_mlp.npkg" baseline
mlp_rc=$?
[ "$mlp_rc" -eq 0 ] || { tail -n 5 "$stem.baseline.log"; fail "baseline nonzero (rc=$mlp_rc)"; }
base_acc="$(extract_acc "$stem.baseline.log")"
[ -n "$base_acc" ] || fail "baseline printed no accuracy line"

# --- 5. mnist_ddp twice -----------------------------------------------
"$ncc" build -p examples/mnist_mlp/mnist_ddp.nproj \
    -o "${stem}_mnist_ddp.npkg" > "$stem.ddpbuild.log" 2>&1 \
    || fail "ddp did not compile (see $stem.ddpbuild.log)"

# occupancy sampler across both ddp runs (step 6 reads its log)
rm -f "$stem.util"
( while :; do
      nvidia-smi --query-gpu=index,utilization.gpu --format=csv,noheader \
          >> "$stem.util" 2>/dev/null || exit 1
      sleep 0.5
  done ) &
sampler_pid=$!
trap 'kill "$sampler_pid" 2>/dev/null' EXIT

train_run "${stem}_mnist_ddp.npkg" ddpA
rcA=$?
train_run "${stem}_mnist_ddp.npkg" ddpB
rcB=$?
kill "$sampler_pid" 2>/dev/null
trap - EXIT

[ "$rcA" -eq 0 ] || { tail -n 5 "$stem.ddpA.log"; fail "ddp run A nonzero (rc=$rcA)"; }
[ "$rcB" -eq 0 ] || { tail -n 5 "$stem.ddpB.log"; fail "ddp run B nonzero (rc=$rcB)"; }
cmp -s <(extract_losses "$stem.ddpA.log") <(extract_losses "$stem.ddpB.log") \
    || fail "ddp loss sequences differ between runs: $(diff \
        <(extract_losses "$stem.ddpA.log") <(extract_losses "$stem.ddpB.log") \
        | head -n 5 | tr '\n' ' ')"

accA="$(extract_acc "$stem.ddpA.log")"
accB="$(extract_acc "$stem.ddpB.log")"
[ -n "$accA" ] && [ -n "$accB" ] || fail "a ddp run printed no accuracy line"
awk -v a="$accA" -v b="$accB" -v base="$base_acc" -v floor="$ACC_FLOOR" \
    -v margin="$ACC_BASE_MARGIN" '
    BEGIN {
        bad = 0
        if (a < floor || b < floor) { printf "accuracy below floor: %s %s < %s\n", a, b, floor; bad = 1 }
        da = a - base; if (da < 0) da = -da
        db = b - base; if (db < 0) db = -db
        if (da > margin || db > margin) { printf "accuracy off baseline: %s %s vs %s (+/-%s)\n", a, b, base, margin; bad = 1 }
        exit bad
    }' || fail "accuracy gate"

# the loss-curve band against the fresh baseline (pairwise on epoch
# index: both runs print one line per epoch, same hyperparameters)
paste <(extract_losses "$stem.baseline.log") \
      <(extract_losses "$stem.ddpA.log") | awk \
    -v band="$DDP_LOSS_BAND" '
    { d = $1 - $2; if (d < 0) d = -d; if (d > m) m = d; if (band+0 > 0 && d > band) over++ }
    END {
        printf "loss-band: maxdev=%.6f over=%d epochs=%d band=%s\n", m, over+0, NR, band
        if (band == "CALIBRATE") exit 0
        exit (over > 0 ? 1 : 0)
    }' || fail "ddp loss curve outside DDP_LOSS_BAND"

# --- 6. both-GPUs-busy check ------------------------------------------
[ -s "$stem.util" ] || fail "nvidia-smi sampler logged nothing ($stem.util)"
awk -F',' '{
        idx = $1; gsub(/ /, "", idx); util = $2 + 0
        seen[idx] = 1; if (util > 0) busy[idx] = 1
      }
      END {
        n = 0; miss = ""
        for (i in seen) { n++; if (!(i in busy)) miss = miss " device" i }
        if (n < 2) { printf "sampler saw %d device indices\n", n; exit 1 }
        if (miss != "") { printf "never busy:%s\n", miss; exit 1 }
      }' "$stem.util" || fail "occupancy (see $stem.util)"

echo "GATE multigpu: PASS (roster double-run identical, ddp accuracies $accA/$accB against fresh baseline $base_acc)"
