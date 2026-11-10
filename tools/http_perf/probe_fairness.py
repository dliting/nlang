"""Fairness probes for the throughput baseline in docs/dev/http_perf_baseline.md.

Two measurements that answer "is that comparison fair?" rather than "what is the
number?", both outside the rig's 120 measured rows:

1. The response head on the wire for a keep-alive 200: nginx against NLang http.n,
   for a 0-byte and a 1 KiB body.
2. What nginx's per-request file lookup costs: the same static path with and
   without `open_file_cache`, run alternately at 1, 4 and 32 connections.

Both reuse the rig's own generated artifacts, so they measure the same cells the
document's table records: the rig's `nginx-off.conf` configuration and its
`srv_nlang.ncu` server. A previous `run_baseline.sh` run has to have produced them
(`PERF_SERVERS="nlang nginx" PERF_LADDER=quick` is enough).

    py313 -I tools/http_perf/probe_fairness.py

Environment overrides are the rig's own names (PERF_OUT, PERF_DUR, PERF_WARM,
PERF_THREADS, PERF_REQ_TIMEOUT, PERF_LOAD_LIMIT, PERF_PORT_NGINX,
PERF_PORT_NLANG, plus NVM, OHA, NGINX, NGINX_PREFIX), so one exported set drives
both programs.

Every measurement needs exactly one server instance on its port, so each start
asserts the port is free and each stop kills the whole process tree. Killing only
nginx's master is not enough and was the defect in the first version of these
probes: on Windows the LISTENING socket belongs to nginx's worker, which survives
its master (verified 2026-11-10: after `Stop-Process -Force` on the master, the
worker was still LISTENING), and the next master binds the same port again instead
of failing, so the stale worker silently takes part of the measured traffic.
"""

import json
import os
import statistics
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def env(name, default):
    return os.environ.get(name) or default


OUT = os.path.abspath(env("PERF_OUT", os.path.join(REPO, "temp", "http_perf")))
NGINX = env("NGINX", r"D:\dev\nginx\nginx-1.31.8\nginx.exe")
NGINX_PREFIX = env("NGINX_PREFIX", r"D:\dev\nginx\nginx-1.31.8")
NVM = env("NVM", os.path.join(REPO, "build-dev", "src", "tools", "nvm",
                              "Release", "nvm.exe"))
OHA = env("OHA", r"D:\dev\oha\oha.exe")
NLANG_SERVER = os.path.join(OUT, "srv_nlang.ncu")
CONF_OFF = os.path.join(OUT, "nginx-off.conf")
CONF_OFC = os.path.join(OUT, "nginx-ofc.conf")
RESULT_PATH = os.path.join(OUT, "fairness_probes.json")

PORT_NGINX = int(env("PERF_PORT_NGINX", "18083"))
PORT_NLANG = int(env("PERF_PORT_NLANG", "18081"))
# Same measurement window as the rig, so the open_file_cache numbers are directly
# comparable with the nginx rows of the document's table.
WARM_S = env("PERF_WARM", "3s")
DUR_S = env("PERF_DUR", "15s")
GEN_THREADS = env("PERF_THREADS", "4")
REQ_TIMEOUT = env("PERF_REQ_TIMEOUT", "10s")
LOAD_LIMIT = int(env("PERF_LOAD_LIMIT", "25"))      # percent, as in the rig
# The open_file_cache directives are inserted after this line of the rig's off
# configuration; both live in the http{} block, which is where nginx wants them.
ANCHOR = "    access_log  off;\n"
OPEN_FILE_CACHE = (
    "    open_file_cache max=20000 inactive=60s;\n"
    "    open_file_cache_valid 60s;\n"
    "    open_file_cache_min_uses 1;\n"
    "    open_file_cache_errors on;\n")


def run(cmd):
    # errors="replace": taskkill and PowerShell answer in the console code page
    # (this box is zh-CN), and the text is only ever read for digits and PIDs.
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def listeners(port):
    """PIDs LISTENING on <port> at any local address (an 0.0.0.0 listener would
    take the traffic too, so 127.0.0.1 alone is not enough to check)."""
    pids = set()
    for line in run(["netstat", "-ano"]).stdout.splitlines():
        fields = line.split()
        if (len(fields) >= 5 and fields[0] == "TCP" and fields[3] == "LISTENING"
                and fields[1].endswith(":%d" % port)):
            pids.add(fields[4])
    return pids


def require_free(port):
    holders = listeners(port)
    if holders:
        raise RuntimeError("port %d is already held by pid(s) %s; free it first"
                           % (port, ", ".join(sorted(holders))))


def win_start(workdir, exe, *args):
    """Start a detached server and return its real Windows PID. Python's Popen
    would do for the master, but the returned handle is not what holds the port."""
    # Single quotes inside a path would close the PowerShell string literal.
    for path in (exe, workdir) + args:
        if "'" in path:
            raise RuntimeError("path contains a single quote: %r" % path)
    launch = "Start-Process -FilePath '%s' -WorkingDirectory '%s' -WindowStyle Hidden" % (
        exe, workdir)
    if args:
        launch += " -ArgumentList " + ",".join("'%s'" % a for a in args)
    out = run(["powershell", "-NoProfile", "-Command",
               launch + " -PassThru | Select-Object -ExpandProperty Id"]).stdout
    pid = out.strip()
    if not pid.isdigit():
        raise RuntimeError("could not start %s: %r" % (exe, out))
    return int(pid)


def wait_ready(port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if listeners(port):
            return
        time.sleep(0.2)
    raise RuntimeError("nothing listening on port %d after %gs" % (port, timeout))


def stop(pid, port):
    """Kill the tree, then confirm the port went silent; a stop that left a
    listener would poison the next measurement rather than this one."""
    if pid:
        run(["taskkill", "/T", "/F", "/PID", str(pid)])
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if not listeners(port):
            return
        time.sleep(0.3)
    raise RuntimeError("port %d still listening after killing pid %d" % (port, pid))


def load_pct():
    out = run(["powershell", "-NoProfile", "-Command",
               "[math]::Round((Get-CimInstance Win32_Processor | Measure-Object"
               " -Property LoadPercentage -Average).Average)"]).stdout.strip()
    return int(out) if out.lstrip("-").isdigit() else None


class Server:
    """Context manager: one server instance, one port, clean start and stop."""

    def __init__(self, kind):
        self.kind = kind
        self.pid = 0

    def __enter__(self):
        require_free(self.port)
        if self.kind == "nginx":
            self.pid = self.start_nginx(CONF_OFF)
        elif self.kind == "nginx_ofc":
            write_ofc_conf()
            self.pid = self.start_nginx(CONF_OFC)
        elif self.kind == "nlang":
            self.pid = win_start(REPO, NVM, "--carriers=4", NLANG_SERVER)
        else:
            raise RuntimeError("unknown server kind %r" % self.kind)
        wait_ready(self.port)
        held = listeners(self.port)
        if len(held) != 1:
            raise RuntimeError("expected exactly one listener on port %d, got %s"
                               % (self.port, sorted(held)))
        return self

    def __exit__(self, *_):
        stop(self.pid, self.port)
        return False

    def start_nginx(self, conf):
        # nginx.conf wants forward slashes (a backslash inside a quoted value is an
        # escape character there), while -p has to name the unpacked nginx tree,
        # which is where nginx keeps its own temp and log directories.
        return win_start(NGINX_PREFIX, NGINX,
                         "-p", NGINX_PREFIX.replace("\\", "/"),
                         "-c", conf.replace("\\", "/"))

    @property
    def port(self):
        return PORT_NGINX if self.kind.startswith("nginx") else PORT_NLANG


def field_lines(head):
    """The header field lines of a response head: status line and blank line out."""
    return [line for line in head.split(b"\r\n")[1:] if line]


def head_dump(port, path, requests=3):
    """Bytes and line count of one keep-alive response head (status line, the
    header fields, and the blank line ending the head). The connection stays open
    across `requests` requests and the body is drained between them, because a
    `Connection: close` answer is not what the keep-alive rows of the baseline
    actually put on the wire."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    heads = []
    try:
        for _ in range(requests):
            sock.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
                          % path).encode("ascii"))
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RuntimeError("connection closed inside the head")
                buf += chunk
            head = buf.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n"
            fields = {}
            for line in field_lines(head):
                name, _, value = line.partition(b":")
                fields[name.strip().lower().decode()] = value.strip().decode()
            remaining = int(fields.get("content-length", "0"))
            remaining -= len(buf) - len(head)
            while remaining > 0:
                chunk = sock.recv(min(remaining, 65536))
                if not chunk:
                    raise RuntimeError("connection closed inside the body")
                remaining -= len(chunk)
            heads.append(head)
    finally:
        sock.close()
    # The heads are not byte-identical across requests: the Date field is stamped
    # per response and can tick a second over inside three requests on one
    # connection. Length and field set are what must not move.
    if len({len(head) for head in heads}) != 1:
        raise RuntimeError("repeated keep-alive heads differ in length: %r"
                           % [len(head) for head in heads])
    names = sorted(line.split(b":")[0].strip().decode()
                   for line in field_lines(heads[0]))
    for head in heads[1:]:
        if sorted(line.split(b":")[0].strip().decode()
                  for line in field_lines(head)) != names:
            raise RuntimeError("repeated keep-alive heads carry different fields")
    head = heads[0]
    return {
        "path": path,
        "head_bytes": len(head),                     # status line + fields + blank
        "head_lines": head.count(b"\r\n"),           # status line + fields + blank
        "header_fields": len(names),
        "fields": names,
    }


def oha_row(port, path, conns):
    """One warm-up run plus one measured run, with the baseline rig's own flags.
    The status-code distribution comes back too: an RPS gain that came from cached
    304s or cached 404s instead of from real responses has to be visible here."""
    base = ["--no-tui", "-c", str(conns), "--worker-threads", GEN_THREADS,
            "-t", REQ_TIMEOUT, "--disable-compression",
            "http://127.0.0.1:%d%s" % (port, path)]
    run([OHA, "-z", WARM_S] + base)
    load = load_pct()
    if load is not None and load > LOAD_LIMIT:
        print("  note: box load %d%% over the guard limit of %d%%" % (load, LOAD_LIMIT))
    tmp = os.path.join(OUT, "_probe_row.json")
    run([OHA, "-z", DUR_S, "--output-format", "json", "-o", tmp] + base)
    with open(tmp, encoding="utf-8") as handle:
        data = json.load(handle)
    os.remove(tmp)
    summary = data["summary"]
    percentiles = data.get("latencyPercentiles") or {}
    return {
        "rps": float(summary["requestsPerSec"]),
        "p50_ms": float(percentiles.get("p50") or 0.0) * 1000.0,
        "mean_ms": float(summary.get("average") or 0.0) * 1000.0,
        "codes": data.get("statusCodeDistribution") or {},
        "errors": data.get("errorDistribution") or {},
        "load_pct": load,
    }


def write_ofc_conf():
    """The baseline's off configuration plus the four open_file_cache directives."""
    with open(CONF_OFF, encoding="utf-8") as handle:
        text = handle.read()
    if text.count(ANCHOR) != 1:
        raise RuntimeError("anchor %r found %d times in %s"
                           % (ANCHOR, text.count(ANCHOR), CONF_OFF))
    if "open_file_cache" in text:
        raise RuntimeError("%s already carries open_file_cache" % CONF_OFF)
    with open(CONF_OFC, "w", encoding="utf-8", newline="") as handle:
        handle.write(text.replace(ANCHOR, ANCHOR + OPEN_FILE_CACHE))


def measure_heads():
    print("== response head of a keep-alive 200 ==")
    results = []
    for kind in ("nginx", "nlang"):
        with Server(kind) as srv:
            for path in ("/b0", "/b1024"):
                entry = head_dump(srv.port, path)
                results.append({"server": kind, **entry})
                print("%-6s %-7s %d bytes, %d head lines (%d header fields) %s"
                      % (kind, path, entry["head_bytes"], entry["head_lines"],
                         entry["header_fields"], entry["fields"]))
    return results


def measure_open_file_cache(conn_list=(1, 4, 32)):
    """Two off/on pairs per connection count, the pair order flipped for every
    other count, so a drift in the box moves the two halves of the pair in the same
    direction and shows up as a spread instead of hiding in the median.

    Three connection counts rather than one, because the two explanations for an
    RPS gain differ here: a per-request cost falls as connections rise (the
    saturated rate is what it is at 4 and at 32 connections), while a scheduling
    gain would only appear once there is a queue to schedule."""
    print("\n== nginx open_file_cache A/B on /b0 ==")
    cells = []
    for index, conns in enumerate(conn_list):
        order = ["nginx", "nginx_ofc", "nginx_ofc", "nginx"]
        if index % 2:
            order = list(reversed(order))
        rows = {"nginx": [], "nginx_ofc": []}
        print("  %d connection(s):" % conns)
        for position, kind in enumerate(order):
            with Server(kind) as srv:
                row = oha_row(srv.port, "/b0", conns)
            rows[kind].append(row)
            print("    round %d %-10s rps=%8.1f p50=%5.2f ms codes=%s errors=%s"
                  % (position + 1, kind, row["rps"], row["p50_ms"],
                     row["codes"], row["errors"]))
        medians = {kind: statistics.median(r["rps"] for r in values)
                   for kind, values in rows.items()}
        medians_p50 = {kind: statistics.median(r["p50_ms"] for r in values)
                       for kind, values in rows.items()}
        delta = (medians["nginx_ofc"] - medians["nginx"]) / medians["nginx"] * 100.0
        print("    median off=%8.1f on=%8.1f  open_file_cache: %+.1f %% RPS,"
              " p50 %.2f ms -> %.2f ms"
              % (medians["nginx"], medians["nginx_ofc"], delta,
                 medians_p50["nginx"], medians_p50["nginx_ofc"]))
        cells.append({"conns": conns, "path": "/b0", "rounds": rows,
                      "median_rps": medians, "median_p50_ms": medians_p50,
                      "delta_pct": delta})
    return cells


def main():
    for path in (NVM, OHA, NGINX, NLANG_SERVER, CONF_OFF):
        if not os.path.exists(path):
            sys.exit("missing prerequisite: %s\n"
                     "Run the rig first (it generates the nginx configuration and"
                     " the NLang server in %s)." % (path, OUT))
    require_free(PORT_NGINX)
    require_free(PORT_NLANG)
    payload = {"started": time.strftime("%Y-%m-%d %H:%M:%S"),
               "heads": measure_heads(),
               "open_file_cache": measure_open_file_cache()}
    with open(RESULT_PATH, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2)
    print("\nwrote %s" % RESULT_PATH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
