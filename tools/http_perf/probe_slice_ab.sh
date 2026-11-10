#!/usr/bin/env bash
# A/B probe for the poll-slice question in docs/dev/http_perf_baseline.md §5.
#
# The baseline document leaves one measurement open: whether NLang's p50 floor
# comes from how the VM schedules I/O waits or from the server's own work. This
# script answers that by measuring the same cells the baseline table records --
# 0-byte body, 1/4/32/128 connections, closed loop, unlimited rate -- against a
# server build, and printing the rows in the same shape as results.md, so two
# builds' rows can be read side by side. The build under test is whatever NVM
# points at: rebuild nvm with a different kIoPollSliceMs, run the probe again
# with a different tag, and the two sets of rows differ only in that setting.
#
# Like every other measurement on this box, run it through the machine lock:
#
#   tools/gate.sh slice-ab temp/t15 bash tools/http_perf/probe_slice_ab.sh A
#
# Tag argument: a short label for this build, used in the output file names so
# runs do not overwrite each other.
#
# Prerequisite: a previous run_baseline.sh run has to have produced
# $PERF_OUT/srv_nlang.ncu (PERF_SERVERS=nlang is enough); the probe measures that
# server rather than generating its own, so the rows are the rig's cells.
#
# Output: one line per connection count (rps, p50, p90, p99) plus the raw oha
# JSON under $PERF_OUT, and the server's own log for the run.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${PERF_BUILD:-build-dev}
OUT=${PERF_OUT:-$ROOT/temp/http_perf}
NVM=${NVM:-$ROOT/$BUILD/src/tools/nvm/Release/nvm.exe}
OHA=${OHA:-D:/dev/oha/oha.exe}
PY313=${PY313:-D:/dev/miniconda3/envs/py313/python.exe}
PORT=${PERF_PORT_NLANG:-18081}
CONNS=${PERF_SLICE_CONNS:-"1 4 32 128"}
CARRIERS=${PERF_CARRIERS:-4}
DUR=${PERF_DUR:-15s}
WARM=${PERF_WARM:-3s}
GEN_THREADS=${PERF_THREADS:-4}
REQ_TIMEOUT=${PERF_REQ_TIMEOUT:-10s}
TAG=${1:?tag}

if [ ! -f "$OUT/srv_nlang.ncu" ]; then
    echo "no $OUT/srv_nlang.ncu; run tools/http_perf/run_baseline.sh with PERF_SERVERS=nlang first" >&2
    exit 1
fi

measure() {   # measure <conns>
    local conns=$1
    local label="slice${TAG}_${conns}c"
    "$OHA" --no-tui -z "$WARM" -c "$conns" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression \
        "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
    "$OHA" --no-tui -z "$DUR" -c "$conns" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression \
        --output-format json -o "$OUT/$label.json" \
        "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
    "$PY313" -I -c "
import json,sys
d=json.load(open(sys.argv[2],encoding='utf-8'))
p=d['latencyPercentiles']; s=d['summary']
def ms(k): return 'n/a' if p.get(k) is None else '%.2f'%(float(p[k])*1000)
print('%-16s rps=%9.0f  p50=%7s ms  p90=%7s ms  p99=%7s ms' % (
    sys.argv[1], float(s['requestsPerSec']), ms('p50'), ms('p90'), ms('p99')))
" "$label" "$OUT/$label.json"
}

"$NVM" "--carriers=$CARRIERS" "$OUT/srv_nlang.ncu" > "$OUT/slice_$TAG.log" 2>&1 &
PROBE=$!
ready=0
for i in $(seq 1 40); do
    curl -sf -o /dev/null "http://127.0.0.1:$PORT/b0" && { ready=1; break; }
    sleep 0.5
done
if [ "$ready" = 0 ]; then
    echo "server never answered on $PORT; see $OUT/slice_$TAG.log" >&2
    kill "$PROBE" > /dev/null 2>&1
    exit 1
fi

for conns in $CONNS; do
    measure "$conns"
done

kill "$PROBE" > /dev/null 2>&1 || taskkill //T //F //IM nvm.exe > /dev/null 2>&1
wait "$PROBE" > /dev/null 2>&1   # reaping it here keeps bash's "Terminated" notice out of the gate's stderr
taskkill //T //F //IM nvm.exe > /dev/null 2>&1
echo "done"
