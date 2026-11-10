#!/usr/bin/env bash
# Rate-spacing probe for the latency floor in docs/dev/http_perf_baseline.md §5.
#
# The request path measures about 0.9 ms when the server is idle, and every
# request carries about 15 ms once it saturates. This script separates "the
# server is slow per request" from "requests queue in front of something that
# releases on a clock tick" by spacing them further apart than one such tick:
# oha's global rate limit (-q QPS) sets how far apart consecutive requests are
# sent. Below the saturation point the rows show the unburdened latency; above
# it the throughput stops rising while the latency jumps to the floor and stays
# there whatever the spacing. That flat step is what points at a timed wait
# rather than at per-request work.
#
# Run it through the machine lock like every other measurement on this box:
#
#   tools/gate.sh pace-ab temp/t15 bash tools/http_perf/probe_pace_ab.sh
#
# Prerequisite: a previous run_baseline.sh run has to have produced
# $PERF_OUT/srv_nlang.ncu (PERF_SERVERS=nlang is enough); the probe measures that
# server rather than generating its own.
#
# Output: one line per rate-limit step (p50, rps, success rate) plus the raw oha
# JSON under $PERF_OUT, and the server's own log for the run. The "成功请求" column
# in the baseline document is rps times the measurement window, not a separate
# measurement.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${PERF_BUILD:-build-dev}
OUT=${PERF_OUT:-$ROOT/temp/http_perf}
NVM=${NVM:-$ROOT/$BUILD/src/tools/nvm/Release/nvm.exe}
OHA=${OHA:-D:/dev/oha/oha.exe}
PY313=${PY313:-D:/dev/miniconda3/envs/py313/python.exe}
PORT=${PERF_PORT_NLANG:-18081}
CONNS=${PERF_PACE_CONNS:-4}
CARRIERS=${PERF_CARRIERS:-4}
DUR=${PERF_DUR:-8s}
WARM=${PERF_WARM:-2s}
GEN_THREADS=${PERF_THREADS:-4}
REQ_TIMEOUT=${PERF_REQ_TIMEOUT:-10s}
QPS_STEPS=${PERF_PACE_QPS:-"10 50 100 200 500 1000 2000"}

if [ ! -f "$OUT/srv_nlang.ncu" ]; then
    echo "no $OUT/srv_nlang.ncu; run tools/http_perf/run_baseline.sh with PERF_SERVERS=nlang first" >&2
    exit 1
fi

measure() {   # measure <tag> <oha extra args...>
    local tag=$1; shift
    "$OHA" --no-tui -z "$WARM" -c "$CONNS" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression "$@" \
        "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
    "$OHA" --no-tui -z "$DUR" -c "$CONNS" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression "$@" \
        --output-format json -o "$OUT/pace_$tag.json" \
        "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
    "$PY313" -I -c "
import json,sys
d=json.load(open(sys.argv[2],encoding='utf-8'))
p=d['latencyPercentiles']; s=d['summary']
print('%-12s p50=%7s ms  rps=%8.1f  ok=%5.1f%%' % (
    sys.argv[1],
    'n/a' if p.get('p50') is None else '%.2f'%(float(p['p50'])*1000),
    float(s['requestsPerSec']), 100.0*float(s['successRate'])))
" "$tag" "$OUT/pace_$tag.json"
}

"$NVM" "--carriers=$CARRIERS" "$OUT/srv_nlang.ncu" > "$OUT/pace_probe.log" 2>&1 &
PROBE=$!
ready=0
for i in $(seq 1 40); do
    curl -sf -o /dev/null "http://127.0.0.1:$PORT/b0" && { ready=1; break; }
    sleep 0.5
done
if [ "$ready" = 0 ]; then
    # Measuring a dead port otherwise yields a table of nulls that reads like a
    # result: refuse instead.
    echo "server never answered on $PORT; see $OUT/pace_probe.log" >&2
    kill "$PROBE" > /dev/null 2>&1
    exit 1
fi

for qps in $QPS_STEPS; do
    measure "q$qps" -q "$qps"
done
measure unlimited

kill "$PROBE" > /dev/null 2>&1 || taskkill //T //F //IM nvm.exe > /dev/null 2>&1
wait "$PROBE" > /dev/null 2>&1   # reaping it here keeps bash's "Terminated" notice out of the gate's stderr
taskkill //T //F //IM nvm.exe > /dev/null 2>&1
echo "done"
