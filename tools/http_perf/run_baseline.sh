#!/usr/bin/env bash
# HTTP throughput baseline: NLang (stdlib/http.n) vs nginx (static file) vs
# Node (http module) vs Go (net/http) vs Rust (axum on hyper/tokio), all on one
# machine over loopback.
#
# What the numbers mean: every row is driven by an EXTERNAL load generator
# (oha), so the server process under test is the only server-side cost being
# measured, and no client code of the same language competes with its own
# server for a carrier. All five servers answer the same fixed-size bodies.
#
# Run it through the machine lock, like every other gate: a measurement is only
# worth what the machine it ran on was doing, and the other worktrees' gates
# share this box.
#
#   tools/gate.sh http-perf temp/t12_perf bash tools/http_perf/run_baseline.sh
#
# The table is printed at the end; it can also be regenerated from the run's
# JSON at any time:
#   py313 -I tools/http_perf/parse_results.py [output-dir]
#
# Prerequisites, every path overridable through the environment (this script
# installs nothing):
#   oha    load generator    https://github.com/hatoo/oha/releases
#   nginx  static files      https://nginx.org/en/download.html (unzip it)
#   node   Node.js           https://nodejs.org/en/download
#   go     Go toolchain      https://go.dev/dl/
#   cargo  Rust toolchain    rustup (the Rust probe pulls axum/tokio from
#                            crates.io the first time it builds, and pins its own
#                            toolchain in srv_rust/rust-toolchain.toml -- current
#                            axum/hyper releases need rustc 1.85 or newer, where
#                            edition 2024 was stabilised, which is newer than a
#                            box's stale default may be. cargo --version goes into
#                            versions.txt, so every table says what built it.)
#   ncc/nvm from this repo's build tree (PERF_BUILD)
#
# The nginx copy this rig was built against is nginx-1.31.8, unzipped from the
# official zip (sha512 12ff3672…0cbbc8; nginx.org publishes a PGP signature for
# the zip but no checksum file, so the archive's identity is cross-checked by
# the binary's own `nginx -v`).
#
# Everything it writes -- generated sources, the doc root, the compiled probes,
# per-row JSON, rows.csv, results.csv/md, versions.txt -- goes to PERF_OUT,
# which is git-ignored; the .in sources here stay the single copy of record.
#
# Method (why the columns are comparable at all):
#   * A fixed 4 generator OS threads for every row, leaving the other cores to
#     the server under test.
#   * Before each row the box's CPU load is sampled and a busy box is waited
#     out; a row whose wait ran out, or whose load could not be read, is
#     recorded as contended.
#   * A warm-up run (discarded) then a measured run.
#   * Pass 1 measures NLang first and Rust last, pass 2 the reverse, so drift
#     in box state cannot favour one server. parse_results.py reports each
#     cell's pass-to-pass spread; a large spread means the run measured the
#     box, not the server.
#   * nginx on Windows uses its own I/O model (weaker than its Linux epoll
#     path), so its column is a Windows-to-Windows comparison, not the headline
#     nginx number.
#   * Rust has no HTTP server in its standard library, so its column is a crate
#     stack (axum on hyper/tokio, the mainstream combination) while every other
#     column is that platform's own HTTP implementation. Read the Rust column as
#     "what a Rust service built this way gets", not as a language shootout.
#   * nginx's stock sendfile=off is measured; sendfile=on gets its own
#     sensitivity row, as does the NLang server at 1 carrier instead of 4.
#
# Environment overrides (defaults shown):
#   PERF_BUILD=build-dev              PERF_OUT=temp/http_perf
#   PERF_SERVERS="nlang node nginx go rust"   subset to measure (nlang1c and
#                                     nginxsf ride along with nlang and nginx)
#   PERF_LADDER=full                  full | small | quick (fewer rows)
#   PERF_DUR=15s  PERF_WARM=3s  PERF_THREADS=4  PERF_REQ_TIMEOUT=10s
#   PERF_LOAD_LIMIT=25  PERF_LOAD_TRIES=20     # percent, 20 x 5 s of waiting
#   PERF_PORT_NLANG=18081  PERF_PORT_NODE=18082  PERF_PORT_NGINX=18083
#   PERF_PORT_GO=18084     PERF_PORT_RUST=18085
#   NCC, NVM, OHA, NODE, NGINX, NGINX_PREFIX, GO, CARGO, PY313
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${PERF_BUILD:-build-dev}
OUT=${PERF_OUT:-$ROOT/temp/http_perf}
SRC=$ROOT/tools/http_perf
NCC=${NCC:-$ROOT/$BUILD/src/tools/ncc/Release/ncc.exe}
NVM=${NVM:-$ROOT/$BUILD/src/tools/nvm/Release/nvm.exe}
OHA=${OHA:-D:/dev/oha/oha.exe}
NODE=${NODE:-D:/dev/nodejs/node.exe}
NGINX=${NGINX:-D:/dev/nginx/nginx-1.31.8/nginx.exe}
NGINX_PREFIX=${NGINX_PREFIX:-D:/dev/nginx/nginx-1.31.8}
GO=${GO:-D:/dev/go/go/bin/go.exe}
CARGO=${CARGO:-cargo}
PY313=${PY313:-D:/dev/miniconda3/envs/py313/python.exe}

DUR=${PERF_DUR:-15s}
WARM=${PERF_WARM:-3s}
GEN_THREADS=${PERF_THREADS:-4}
REQ_TIMEOUT=${PERF_REQ_TIMEOUT:-10s}
LOAD_LIMIT=${PERF_LOAD_LIMIT:-25}
LOAD_WAIT_TRIES=${PERF_LOAD_TRIES:-20}
LADDER=${PERF_LADDER:-full}
SERVERS=${PERF_SERVERS:-"nlang node nginx go rust"}

PORT_NLANG=${PERF_PORT_NLANG:-18081}
PORT_NODE=${PERF_PORT_NODE:-18082}
PORT_NGINX=${PERF_PORT_NGINX:-18083}
PORT_GO=${PERF_PORT_GO:-18084}
PORT_RUST=${PERF_PORT_RUST:-18085}

DOC_ROOT=$OUT/html
CUR_PID=""

mkdir -p "$DOC_ROOT"

log() { echo "$(date +%H:%M:%S) $*" >> "$OUT/run.log"; }
die() { echo "http_perf: $*" >&2; exit 1; }

die_log() {   # die_log <what failed> <log file> -- quote the log's tail instead
              # of only naming the file: an external build's real error (cargo's
              # edition2024 complaint, say) is otherwise invisible until someone
              # opens the file.
    echo "http_perf: $1 failed, see $2" >&2
    tail -8 "$2" 2>/dev/null | tr -d '\r' | sed 's/^/   /' >&2
    exit 1
}

# One run per output directory: two instances would append to the same rows.csv
# and overwrite each other's per-row JSON mid-write, which reads as doubled rows
# and half-written files rather than as two runs. The machine lock in
# tools/gate.sh serialises this run against the other gates; this lock covers
# two copies of the rig itself, and lives in PERF_OUT so parallel output
# directories stay parallel.
if ! mkdir "$OUT/.lock" 2>/dev/null; then
    die "$OUT/.lock exists: another run is under way, or a killed run left it
   behind (remove that directory to take over)"
fi
# The parser reads every *.json in the directory, so a rerun must not inherit the
# previous run's samples.
rm -f "$OUT"/*.json "$OUT/results.csv" "$OUT/results.md"
: > "$OUT/rows.csv"
: > "$OUT/run.log"

# ---------------------------------------------------------------- prerequisites

require_exe() {   # require_exe <path> <what it is> <where to get it>
    [ -f "$1" ] || die "$2 not found: $1
   $3
   Point the matching environment variable at it, or edit the default."
}

require_cmd() {   # require_cmd <name-or-path> <what> <where> -- bare names are
                   # looked up on PATH, anything else must be a real file.
    case "$1" in
        *[\\/]) require_exe "$1" "$2" "$3" ;;
        *) command -v "$1" > /dev/null 2>&1 \
               || die "$2 not found on PATH: $1
   $3" ;;
    esac
}

check_tools() {
    require_cmd cygpath "cygpath" "ships with Git for Windows; the probes are handed to PowerShell, which needs Windows-shaped paths"
    require_exe "$NCC" "ncc" "Build it first: cmake --build $BUILD --config Release"
    require_exe "$NVM" "nvm" "Build it first: cmake --build $BUILD --config Release"
    require_exe "$OHA" "oha load generator" "https://github.com/hatoo/oha/releases"
    if selected node;  then require_cmd "$NODE"  "node"  "https://nodejs.org/en/download"; fi
    if selected nginx; then require_cmd "$NGINX" "nginx" "https://nginx.org/en/download.html (unzip it, don't install the service)"; fi
    if selected go;    then require_cmd "$GO"    "go"    "https://go.dev/dl/"; fi
    if selected rust;  then require_cmd "$CARGO" "cargo" "https://www.rust-lang.org/tools/install (it also needs network access to crates.io on a cold cache)"; fi
}

selected() {   # selected <label> -- the sensitivity servers ride with their base
    local base
    case "${1:-}" in
        nlang1c) base=nlang ;;
        nginxsf) base=nginx ;;
        *)       base="${1:-}" ;;
    esac
    [[ " $SERVERS " == *" $base "* ]]
}

port_free() {   # port_free <port> -- refuse to measure a port someone else holds
                # (any local address, not just 127.0.0.1: an 0.0.0.0 listener
                # would take the traffic and the column would read as ours).
    local port="$1"
    if netstat -ano 2>/dev/null | tr -d '\r' \
            | grep -Eq ":$port[[:space:]].*LISTENING"; then
        die "port $port is already listening; free it or set PERF_PORT_* (every probe binds 127.0.0.1)"
    fi
}

# ------------------------------------------------------------------- processes

winpath() {   # winpath <path> -- Git-Bash form to Windows form with backslashes,
              # what Start-Process and a native exe's own file arguments want.
              # The conversion is not cosmetic: Start-Process sees the literal
              # text of -Command, so it gets none of the argv translation Git Bash
              # applies when a native program is exec'd directly, and a mount-form
              # path like /e/cases/... is simply not a path it can resolve.
    cygpath -w "$1"
}

winfwd() {    # winfwd <path> -- the same path with forward slashes: nginx.conf
              # reads a backslash inside a quoted value as an escape character, so
              # its paths have to stay slash-forward (see nginx.conf.in).
    cygpath -m "$1"
}

win_start() {   # win_start <working-dir> <exe> [arg...] -> Windows pid on stdout
    local wd exe a args=""
    wd=$(winpath "$1"); shift
    exe=$(winpath "$1"); shift
    local launch="Start-Process -FilePath '$exe'"
    for a in "$@"; do
        # Only mount-form arguments are paths here (--carriers=4 is not, and
        # cygpath would read its dashes as options).
        case "$a" in
            /*) a=$(winpath "$a") ;;
        esac
        args+="'$a',"
    done
    if [ -n "$args" ]; then
        launch="$launch -ArgumentList ${args%,}"
    fi
    # -WindowStyle Hidden keeps a console-subsystem server from stealing the
    # foreground, which the desktop-owning gates of another worktree need.
    powershell -NoProfile -Command "$launch -WorkingDirectory '$wd' -WindowStyle Hidden -PassThru | Select-Object -ExpandProperty Id" \
        | tr -d '\r\n'
}

render() {   # render <template> <out> key=value... -- plain literal
              # substitution rather than sed: the substituted values carry
              # backslashes and slashes, which sed replacement text mangles.
    local tpl="$1" out="$2"; shift 2
    local text pair key value
    text=$(< "$tpl")
    for pair in "$@"; do
        key=${pair%%=*}; value=${pair#*=}
        text=${text//$key/$value}
    done
    printf '%s\n' "$text" > "$out"
}

stop() {   # stop <pid> -- the whole tree; a dead or empty pid is not an error
    [ -n "${1:-}" ] || return 0
    taskkill //T //F //PID "$1" > /dev/null 2>&1
    sleep 1
}

cleanup() {
    stop "$CUR_PID"
    rmdir "$OUT/.lock" 2>/dev/null   # mine to remove: only the owner reaches here
}
trap cleanup EXIT INT TERM

wait_ready() {   # wait_ready <port> -- the listener answers before we measure
    local port="$1" i
    for i in $(seq 1 40); do
        if curl -sf -o /dev/null "http://127.0.0.1:$port/b0"; then return 0; fi
        sleep 0.5
    done
    return 1
}

# ---------------------------------------------------- build the NLang/Go/Rust
# probes once, so no compile lands inside a measured window.

build_probes() {
    if selected nlang; then
        render "$SRC/srv_nlang.n.in" "$OUT/srv_nlang.n" "__PORT__=$PORT_NLANG"
        "$NCC" build "$OUT/srv_nlang.n" -o "$OUT/srv_nlang.ncu" > "$OUT/ncc.log" 2>&1 \
            || die_log "ncc build of the probe" "$OUT/ncc.log"
    fi
    if selected node; then
        render "$SRC/srv_node.js.in" "$OUT/srv_node.js" "__PORT__=$PORT_NODE"
    fi
    if selected go; then
        render "$SRC/srv_go.go.in" "$OUT/srv_go.go" "__PORT__=$PORT_GO"
        # GOTOOLCHAIN=local: a missing module file must not send go looking for
        # a newer toolchain on the network mid-run.
        GOTOOLCHAIN=local "$GO" build -o "$OUT/srv_go.exe" "$OUT/srv_go.go" > "$OUT/go.log" 2>&1 \
            || die_log "go build" "$OUT/go.log"
    fi
    if selected rust; then
        # A cargo project, rendered into PERF_OUT so the port can be
        # substituted; Cargo.toml (and the lock file, once one is committed)
        # stays the copy of record here in tools/http_perf/srv_rust.
        mkdir -p "$OUT/rust/src"
        cp "$SRC/srv_rust/Cargo.toml" "$SRC/srv_rust/rust-toolchain.toml" "$OUT/rust/"
        local locked="" cargo_lock="$SRC/srv_rust/Cargo.lock"
        if [ -f "$cargo_lock" ]; then
            cp "$cargo_lock" "$OUT/rust/Cargo.lock"; locked="--locked"
        fi
        render "$SRC/srv_rust/main.rs.in" "$OUT/rust/src/main.rs" "__PORT__=$PORT_RUST"
        # Built from inside the crate directory: rustup reads the pinned
        # toolchain from rust-toolchain.toml there, and --manifest-path would not
        # show it that file. --locked when a lock file ships, and the target
        # directory lives in PERF_OUT: the crate versions and the crate build a
        # table was produced with both stay reusable for a rerun.
        ( cd "$OUT/rust" && "$CARGO" build --release $locked \
              --target-dir "$OUT/rust/target" ) > "$OUT/cargo.log" 2>&1 \
            || die_log "cargo build" "$OUT/cargo.log"
    fi
}

# ------------------------------------------------------------------- measuring

load_pct() {
    # WMI rather than Get-Counter: the performance-counter names are localized
    # (this box runs a zh-CN Windows), so the English '\Processor(_Total)\...'
    # path would fail and silently leave the guard disabled.
    powershell -NoProfile -Command '[math]::Round((Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average)' 2>/dev/null | tr -d '\r\n '
}

row() {   # row <label> <port> <path> <conns> [extra oha flags...]
    local label="$1" port="$2" path="$3" conn="$4"; shift 4
    local load="" tries=0 contended=0
    while : ; do
        load=$(load_pct)
        [[ "$load" =~ ^[0-9]+$ ]] && [ "$load" -le "$LOAD_LIMIT" ] && break
        [ "$tries" -ge "$LOAD_WAIT_TRIES" ] && break
        tries=$((tries + 1)); sleep 5
    done
    if [[ ! "$load" =~ ^[0-9]+$ ]]; then
        load=na; contended=1          # guard could not read the box: suspect row
    elif [ "$load" -gt "$LOAD_LIMIT" ]; then
        contended=1
    fi
    "$OHA" --no-tui -z "$WARM" -c "$conn" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression \
        "http://127.0.0.1:$port$path" > /dev/null 2>&1
    "$OHA" --no-tui -z "$DUR" -c "$conn" --worker-threads "$GEN_THREADS" \
        -t "$REQ_TIMEOUT" --disable-compression --output-format json \
        -o "$OUT/$label.json" "$@" "http://127.0.0.1:$port$path" > /dev/null 2>&1
    local rc=$?
    echo "$label,$load,$contended,$rc" >> "$OUT/rows.csv"
    log "row $label load=$load contended=$contended rc=$rc"
}

ladder() {   # ladder <mode> <label> <port>
    #   quick: one row per body size, for checking the rig works
    #   small: the two connection counts that matter, for a sensitivity server
    #   full:  the request-cost sizes at 1/4/32/128 connections, the 8 KiB body
    #          at a small and an oversubscribed pool, plus one row with
    #          keep-alive off (the new-connection-per-request cost)
    local mode="$1" label="$2" port="$3" size conn
    case "$mode" in
        quick)
            for size in 0 1024 8192; do
                row "${label}_${size}b_4c_p$PASS" "$port" "/b$size" 4
            done
            return ;;
        small)
            for size in 0 1024; do
                for conn in 4 32; do
                    row "${label}_${size}b_${conn}c_p$PASS" "$port" "/b$size" "$conn"
                done
            done
            return ;;
    esac
    for size in 0 1024; do
        for conn in 1 4 32 128; do
            row "${label}_${size}b_${conn}c_p$PASS" "$port" "/b$size" "$conn"
        done
    done
    for conn in 4 128; do
        row "${label}_8192b_${conn}c_p$PASS" "$port" /b8192 "$conn"
    done
    row "${label}_0b_32c_nok_p$PASS" "$port" /b0 32 --disable-keepalive
}

# -------------------------------------------------------------------- servers

start_nginx() {   # start_nginx <sendfile on|off>
    # The doc root is generated rather than committed: three bodies of the same
    # sizes the other servers answer.
    : > "$DOC_ROOT/b0"
    head -c 1024 /dev/zero | tr '\0' 'a' > "$DOC_ROOT/b1024"
    head -c 8192 /dev/zero | tr '\0' 'a' > "$DOC_ROOT/b8192"
    local conf="$OUT/nginx-$1.conf"
    render "$SRC/nginx.conf.in" "$conf" \
        "__PORT__=$PORT_NGINX" "__SENDFILE__=$1" "__ROOT__=$(winfwd "$DOC_ROOT")"
    win_start "$NGINX_PREFIX" "$NGINX" -p "$(winfwd "$NGINX_PREFIX")" \
        -c "$(winfwd "$conf")"
}

run_server() {   # run_server <label> -- start one server, measure it, stop it
    local label="$1" port exe carriers=4 mode=$LADDER sendfile=off
    case "$label" in
        nlang)   port=$PORT_NLANG; exe="$OUT/srv_nlang.ncu" ;;
        nlang1c) # Sensitivity run: what the 4 carriers buy over 1, so it needs
                 # only the connection counts that show the difference.
                 port=$PORT_NLANG; exe="$OUT/srv_nlang.ncu"; carriers=1
                 [ "$mode" = full ] && mode=small ;;
        node)    port=$PORT_NODE;  exe="$OUT/srv_node.js" ;;
        go)      port=$PORT_GO;    exe="$OUT/srv_go.exe" ;;
        rust)    port=$PORT_RUST;  exe="$OUT/rust/target/release/srv_rust.exe" ;;
        nginx)   port=$PORT_NGINX; sendfile=off ;;
        nginxsf) port=$PORT_NGINX; sendfile=on ;;
        *) die "unknown server label: $label" ;;
    esac
    port_free "$port"
    case "$label" in
        nlang*) CUR_PID=$(win_start "$ROOT" "$NVM" "--carriers=$carriers" "$exe") ;;
        node)   CUR_PID=$(win_start "$ROOT" "$NODE" "$exe") ;;
        go)     CUR_PID=$(win_start "$OUT" "$exe") ;;
        rust)   CUR_PID=$(win_start "$OUT" "$exe") ;;
        *)      CUR_PID=$(start_nginx "$sendfile") ;;
    esac
    log "$label pid=$CUR_PID"
    if ! wait_ready "$port"; then
        log "FAIL $label: no answer on port $port"
        stop "$CUR_PID"; CUR_PID=""; return 1
    fi
    if [ "$label" = nginxsf ] && [ "$LADDER" = full ]; then
        row "${label}_0b_32c_p$PASS" "$port" /b0 32     # sensitivity row only
    else
        ladder "$mode" "$label" "$port"
    fi
    stop "$CUR_PID"; CUR_PID=""
}

# ------------------------------------------------------------------------ main

tool_version() {   # tool_version <label> <display> <cmd...> -- one version line
                   # per selected toolchain; a table should say what built it.
    selected "$1" || return 0
    shift
    local display="$1"; shift
    echo "$display: $("${@:1}" 2>&1 | head -1)"
}

record_versions() {
    {
        echo "oha: $("$OHA" --version 2>&1 | head -1)"
        tool_version node "node" "$NODE" --version
        tool_version nginx "nginx" "$NGINX" -v
        tool_version go "go" env GOTOOLCHAIN=local "$GO" version
        # Asked from inside the generated crate: rustup picks the toolchain pinned
        # in rust-toolchain.toml there, and the box's default cargo is not what
        # built the probe. This file is a table's provenance, so it has to name
        # the compiler that ran.
        if [ -d "$OUT/rust" ]; then
            ( cd "$OUT/rust" && tool_version rust "cargo" "$CARGO" --version )
        else
            tool_version rust "cargo" "$CARGO" --version
        fi
        echo "ncc: $NCC"
        echo "nvm: $NVM"
        echo "git: $(git -C "$ROOT" rev-parse --short HEAD 2>&1)"
        echo "date: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "windows: $(powershell -NoProfile -Command '$o = Get-CimInstance Win32_OperatingSystem; $o.Caption + " build " + $o.BuildNumber' 2>&1 | tr -d '\r')"
        # The machine, not only its core count: a throughput table means nothing
        # without the CPU it came from.
        echo "cpu: $(powershell -NoProfile -Command '(Get-CimInstance Win32_Processor).Name -join ", "' 2>&1 | tr -d '\r\n')"
        echo "logical cpus: ${NUMBER_OF_PROCESSORS:-unknown}"
        echo "servers: $SERVERS"
        echo "ladder: $LADDER  dur=$DUR warm=$WARM gen_threads=$GEN_THREADS"
        echo "ports: nlang=$PORT_NLANG node=$PORT_NODE nginx=$PORT_NGINX go=$PORT_GO rust=$PORT_RUST"
    } > "$OUT/versions.txt"
}

# Pass 1 measures NLang first and Rust last, pass 2 the other way round: no
# server always runs on a colder or warmer box than another.
PASS1="nlang node nginx go rust"
PASS2="rust go nginx node nlang"
EXTRA="nlang1c nginxsf"        # sensitivity runs, same position in both passes

check_tools
build_probes
record_versions
for PASS in 1 2; do
    log "=== pass $PASS start"
    order=$([ "$PASS" = 1 ] && echo "$PASS1" || echo "$PASS2")
    for label in $order $EXTRA; do
        if ! selected "$label"; then log "skip $label (PERF_SERVERS=$SERVERS)"; continue; fi
        run_server "$label" || true
    done
    log "=== pass $PASS done"
done
log "ALL DONE"
echo "http_perf: wrote $OUT/rows.csv and $OUT/*.json"
if [ -f "$PY313" ]; then
    "$PY313" -I "$SRC/parse_results.py" "$OUT" \
        || echo "http_perf: parse_results.py failed; run it by hand" >&2
else
    echo "http_perf: PY313 not found ($PY313); run tools/http_perf/parse_results.py yourself" >&2
fi
