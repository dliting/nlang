#!/usr/bin/env bash
# E4: the raw-socket floor for the ablation ladder in
# docs/dev/http_perf_baseline.md §5.
#
# The slice probe measures the shipped http.n server; this one measures the
# same cells against a net-only server that answers a fixed 0-byte response,
# so the gap between the two row sets is everything http.n and the script
# layer cost, and this probe's own rows are the floor the parking path cannot
# go below. Rows come out in results.md's shape so they read side by side.
#
#   tools/gate.sh e4-raw temp/t22 bash tools/http_perf/probe_raw_ab.sh base
#
# Tag argument: a short label used in the output file names so runs do not
# overwrite each other. It is required (TAG=${1:?tag}, the same discipline as
# probe_slice_ab.sh): tag the unpatched arm "base" and a later re-run by what
# changed, so two row sets can never be mistaken for each other.
#
# Output: one line per connection count (rps, p50, p90, p99), the raw oha JSON
# under $PERF_OUT, and the probe's own log for the run.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${PERF_BUILD:-build-dev}
OUT=${PERF_OUT:-$ROOT/temp/http_perf}
NCC=${NCC:-$ROOT/$BUILD/src/tools/ncc/Release/ncc.exe}
NVM=${NVM:-$ROOT/$BUILD/src/tools/nvm/Release/nvm.exe}
OHA=${OHA:-D:/dev/oha/oha.exe}
PY313=${PY313:-D:/dev/miniconda3/envs/py313/python.exe}
PORT=${PERF_RAW_PORT:-18086}
CONNS=${PERF_RAW_CONNS:-"1 4 32 128"}
CARRIERS=${PERF_CARRIERS:-4}
DUR=${PERF_DUR:-15s}
WARM=${PERF_WARM:-3s}
GEN_THREADS=${PERF_THREADS:-4}
REQ_TIMEOUT=${PERF_REQ_TIMEOUT:-10s}
TAG=${1:?tag}
mkdir -p "$OUT"

# Render the port into the source and compile it (the same recipe as
# run_baseline.sh:262-297 for the baseline probe).
sed "s/__PORT__/$PORT/" "$ROOT/tools/http_perf/probe_raw_server.n.in" > "$OUT/probe_raw.n"
"$NCC" build "$OUT/probe_raw.n" -o "$OUT/probe_raw.ncu" > "$OUT/ncc_raw.log" 2>&1
if [ ! -f "$OUT/probe_raw.ncu" ]; then
    echo "ncc build of the raw probe failed; see $OUT/ncc_raw.log" >&2
    tail -40 "$OUT/ncc_raw.log" >&2
    exit 1
fi

measure() {   # measure <conns>
    local conns=$1
    local label="raw${TAG}_${conns}c"
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

"$NVM" "--carriers=$CARRIERS" "$OUT/probe_raw.ncu" > "$OUT/raw_$TAG.log" 2>&1 &
PROBE=$!
ready=0
for i in $(seq 1 40); do
    curl -sf -o /dev/null "http://127.0.0.1:$PORT/b0" && { ready=1; break; }
    sleep 0.5
done
if [ "$ready" = 0 ]; then
    echo "probe never answered on $PORT; see $OUT/raw_$TAG.log" >&2
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
