#!/usr/bin/env bash
# Experiment E0 of docs/dev/net_http_perf_design.md §3.3: does the flat ~15 ms
# per request survive when the virtual machine is taken out altogether?
#
# This script measures the two pure-C++ control-group binaries that
# tools/http_perf/build_control.ps1 produces — one that registers poll interest
# at the poller's round boundary (today's shape) and one that registers it at
# the park site (ruling D1's shape) — against the same cells the baseline
# document's main table records: 0-byte body, 1/4/32/128 connections, closed
# loop, unlimited rate, two passes per cell with the median reported. The rows
# are printed in the same shape as probe_slice_ab.sh does, so they can be read
# against the baseline's rows (1 connection 64.5 RPS, 4 connections 321, 32
# connections 2501, 128 connections 5853) and against each other.
#
# Reading the two sets of rows (the verdict matrix is §3.3's "E0判决矩阵"): a
# step in the round mode that the immediate mode does not show puts the
# quantum in the registration shape itself, i.e. in the native layer, with the
# VM exonerated before any VM code is touched. Neither showing it means our
# native code does not produce it and the cause is on the VM side. Both
# showing it means the cause is below the registration shape, in wepoll/AFD or
# the TCP interaction.
#
# Run it through the machine lock, like every other measurement on this box:
#
#   tools/gate.sh e0-control temp/t16 bash tools/http_perf/probe_control_ab.sh
#
# Prerequisite: the two binaries exist. Build them first —
#   powershell -NoProfile -File tools/http_perf/build_control.ps1
#
# Environment knobs: PERF_CONTROL_DIR (binary directory), PERF_CONTROL_MODES
# (which shapes to run), PERF_CONTROL_CONNS, PERF_CONTROL_PORT, PERF_PASSES,
# and the shared PERF_DUR / PERF_WARM / PERF_THREADS / PERF_REQ_TIMEOUT.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${PERF_OUT:-$ROOT/temp/http_perf}
CTL_DIR=${PERF_CONTROL_DIR:-$OUT/control}
OHA=${OHA:-D:/dev/oha/oha.exe}
PY313=${PY313:-D:/dev/miniconda3/envs/py313/python.exe}
PORT_BASE=${PERF_CONTROL_PORT:-18091}
CONNS=${PERF_CONTROL_CONNS:-"1 4 32 128"}
MODES=${PERF_CONTROL_MODES:-"round immediate"}
PASSES=${PERF_PASSES:-2}
DUR=${PERF_DUR:-15s}
WARM=${PERF_WARM:-3s}
GEN_THREADS=${PERF_THREADS:-4}
REQ_TIMEOUT=${PERF_REQ_TIMEOUT:-10s}

for mode in $MODES; do
    if [ ! -f "$CTL_DIR/control_$mode.exe" ]; then
        echo "no $CTL_DIR/control_$mode.exe; run tools/http_perf/build_control.ps1 first" >&2
        exit 1
    fi
done

#One line per pass, then the median over the passes (two passes: the median of
#two samples is their midpoint, which is what parse_results.py does for the
#baseline table too). Percentiles come back in seconds.
report() {   # report <label> <json> [<json> ...]
    "$PY313" -I -c "
import json,statistics,sys
label,files=sys.argv[1],sys.argv[2:]
rows=[]
for f in files:
    d=json.load(open(f,encoding='utf-8'))
    p,s=d['latencyPercentiles'],d['summary']
    rows.append((float(s['requestsPerSec']),p.get('p50'),p.get('p90'),p.get('p99')))
def ms(v): return 'n/a' if v is None else '%.2f'%(float(v)*1000)
for i,r in enumerate(rows,1):
    print('%-24s pass%d rps=%8.0f  p50=%7s ms  p90=%7s ms  p99=%7s ms'
          % (label,i,r[0],ms(r[1]),ms(r[2]),ms(r[3])))
def med(col):
    vals=[r[col] for r in rows if r[col] is not None]
    return statistics.median(vals) if vals else None
m=[med(c) for c in range(4)]
print('%-24s median rps=%8.0f  p50=%7s ms  p90=%7s ms  p99=%7s ms'
      % (label,m[0] or 0,ms(m[1]),ms(m[2]),ms(m[3])))
" "$@" 2>&1
}

modeIndex=0
for mode in $MODES; do
    #One port per shape. A listener whose port was just in use can refuse to
    #bind again (bind without SO_REUSEADDR, which NetBind does not set, fails
    #with WSAEADDRINUSE while any socket still names that local port), and a
    #server that silently failed to start while the previous shape's process
    #lives on would put both sets of rows on the same binary. The server
    #prints its own shape, and that line is required before anything runs.
    PORT=$((PORT_BASE + modeIndex))
    modeIndex=$((modeIndex + 1))
    echo "=== control_$mode.exe on port $PORT ==="
    "$CTL_DIR/control_$mode.exe" --port="$PORT" > "$OUT/e0_$mode.log" 2>&1 &
    PROBE=$!
    ready=0
    for i in $(seq 1 40); do
        grep -q "mode=$mode" "$OUT/e0_$mode.log" 2>/dev/null \
            && curl -sf -o /dev/null "http://127.0.0.1:$PORT/b0" && { ready=1; break; }
        sleep 0.5
    done
    if [ "$ready" = 0 ]; then
        echo "no server answering as mode=$mode on port $PORT; see $OUT/e0_$mode.log" >&2
        kill "$PROBE" > /dev/null 2>&1
        taskkill //T //F //IM "control_$mode.exe" > /dev/null 2>&1
        exit 1
    fi
    for conns in $CONNS; do
        label="e0_${mode}_${conns}c"
        jsons=""
        for pass in $(seq 1 "$PASSES"); do
            "$OHA" --no-tui -z "$WARM" -c "$conns" --worker-threads "$GEN_THREADS" \
                -t "$REQ_TIMEOUT" --disable-compression \
                "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
            "$OHA" --no-tui -z "$DUR" -c "$conns" --worker-threads "$GEN_THREADS" \
                -t "$REQ_TIMEOUT" --disable-compression \
                --output-format json -o "$OUT/$label-p$pass.json" \
                "http://127.0.0.1:$PORT/b0" > /dev/null 2>&1
            jsons="$jsons $OUT/$label-p$pass.json"
        done
        # shellcheck disable=SC2086   # the json list is deliberately word-split
        report "$label" $jsons
    done
    kill "$PROBE" > /dev/null 2>&1
    wait "$PROBE" > /dev/null 2>&1   # reaping it here keeps bash's "Terminated"
                                     # notice out of the gate's stderr
    #Teardown is verified rather than trusted: a survivor would answer the
    #next shape's port-freeing bind and, worse, could answer the client while
    #the next shape's process is the one supposedly serving.
    for i in $(seq 1 10); do
        tasklist //FI "IMAGENAME eq control_$mode.exe" 2>/dev/null \
            | grep -qi "control_$mode" || break
        taskkill //T //F //IM "control_$mode.exe" > /dev/null 2>&1
        sleep 0.5
    done
done
echo "done"
