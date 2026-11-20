#!/usr/bin/env bash
# Time-interleaved paired A/B on the paced probe (docs/dev/http_perf_baseline.md
# §5, "D9 level-0 landing recheck"), for effects below about 100 microseconds.
#
# Block-sequential A/B rounds cannot measure effects that small on this box:
# two rounds of the SAME build drifted 0.14 ms apart while the effect under
# test was 0.04 ms. Alternating the two sides within each pair puts the two
# measurements back to back in time, so machine-level drift cancels in the
# per-pair differences and the noise floor drops to about 0.012 ms.
#
# Only stdlib/http.n differs between the arms: the server unit loads the
# stdlib package at runtime, so rebuilding that one target switches the
# server; the compiler and the VM stay at whatever the build tree holds.
#
# Run it bare — it takes the machine lock itself, once per probe run, through
# tools/gate.sh, and rebuilds as a --shared build gate:
#
#   bash tools/http_perf/probe_pair_ab.sh <base-ref> <new-ref> <out-root> [pairs]
#
# Prerequisite: a previous run_baseline.sh run with PERF_SERVERS=nlang has
# produced temp/http_perf/srv_nlang.ncu. Each probe writes its oha JSON to
# <out-root>/p<i>/<arm>/pace_q200.json; summarize with
#
#   py313 -I tools/http_perf/pair_summary.py <out-root>
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BASE=${1:?usage: probe_pair_ab.sh <base-ref> <new-ref> <out-root> [pairs]}
NEW=${2:?usage: probe_pair_ab.sh <base-ref> <new-ref> <out-root> [pairs]}
OUT=${3:?usage: probe_pair_ab.sh <base-ref> <new-ref> <out-root> [pairs]}
PAIRS=${4:-5}
BUILD=${PERF_BUILD:-build-dev}
QPS=${PERF_PACE_QPS:-200}
SRV=$ROOT/temp/http_perf/srv_nlang.ncu
PREFIX=$(basename "$OUT")
case "$OUT" in /*) ;; *) OUT=$ROOT/$OUT ;; esac

if [ ! -f "$SRV" ]; then
    echo "no $SRV; run tools/http_perf/run_baseline.sh with PERF_SERVERS=nlang first" >&2
    exit 1
fi
# The arm switch rewrites stdlib/http.n in the worktree; uncommitted edits to
# that same file would silently join one arm, so refuse to start over them.
if [ -n "$(git -C "$ROOT" status --porcelain stdlib/http.n)" ]; then
    echo "stdlib/http.n has uncommitted edits; commit or revert them first" >&2
    exit 1
fi

build_npkg() {   # build_npkg <gate-tag-prefix>
    "$ROOT/tools/gate.sh" --shared "$1" "$OUT/npkg" \
        cmake --build "$BUILD" --config Release --target nlang_stdlib_package
}

run_probe() {   # run_probe <arm-dir> <gate-tag>
    local dir=$1 tag=$2
    mkdir -p "$dir" && cp "$SRV" "$dir/"
    PERF_PACE_QPS=$QPS PERF_OUT="$dir" \
        "$ROOT/tools/gate.sh" "$tag" "$dir/gate" \
        bash "$ROOT/tools/http_perf/probe_pace_ab.sh"
    local rc=$?
    grep -a "GATE $tag: PASS" "$dir/gate.out" > /dev/null && [ "$rc" = 0 ] \
        || { echo "probe $tag failed (rc=$rc), see $dir/gate.out" >&2; exit 1; }
    echo "$tag PASS"
}

mkdir -p "$OUT"
for i in $(seq 1 "$PAIRS"); do
    git -C "$ROOT" checkout "$BASE" -- stdlib/http.n && build_npkg "npkg-b$i"
    run_probe "$OUT/p$i/b" "${PREFIX}${i}b"
    git -C "$ROOT" checkout "$NEW" -- stdlib/http.n && build_npkg "npkg-n$i"
    run_probe "$OUT/p$i/n" "${PREFIX}${i}n"
done
# Leave the worktree and the stdlib package on the new side, as if the loop
# had never edited stdlib/http.n.
git -C "$ROOT" checkout "$NEW" -- stdlib/http.n && build_npkg "npkg-restore"
echo "pairs=$PAIRS out=$OUT"
