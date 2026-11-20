#!/usr/bin/env python3
"""Turn a tools/http_perf run's oha JSON files into a CSV and comparison tables.

Usage: py313 -I tools/http_perf/parse_results.py [output-dir]
       (default: <repo>/temp/http_perf)

Rows are keyed by their file name, <server>_<size>b_<conn>c[_nok]_p<pass>.json,
and joined to rows.csv (pre-row CPU load, contended flag, generator exit code).
Two passes of the same cell are aggregated (median RPS, mean latency) and the
pass-to-pass spread is reported, because a large spread means the box, not the
server, is what the run measured.

oha's JSON times are in SECONDS regardless of --time-unit (checked against a
live run), so everything here is converted to milliseconds. The successful
request count comes from statusCodeDistribution, because this oha version's
"summary.total" is the elapsed seconds, not a request count.
"""

import csv
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LABEL_RE = re.compile(r"^(?P<server>[a-z0-9]+)_(?P<size>\d+)b_(?P<conn>\d+)c"
                      r"(?P<mode>_nok)?_p(?P<pass>\d+)\.json$")
SERVER_ORDER = ["nlang", "nlang1c", "node", "nginx", "nginxsf", "go", "rust"]
SERVER_LABEL = {
    "nlang": "NLang http.n (4 carriers)",
    "nlang1c": "NLang http.n (1 carrier)",
    "node": "Node http",
    "nginx": "nginx static (sendfile off)",
    "nginxsf": "nginx static (sendfile on)",
    "go": "Go net/http",
    "rust": "Rust axum/tokio",
}
BASELINE_ORDER = ["nginx", "node", "go", "rust"]


def read_meta(out_dir):
    meta = {}
    path = os.path.join(out_dir, "rows.csv")
    if not os.path.exists(path):
        return meta
    with open(path, encoding="utf-8") as handle:
        for fields in csv.reader(handle):
            if len(fields) == 4:
                meta[fields[0]] = {
                    "load_pct": fields[1], "contended": fields[2], "rc": fields[3]}
    return meta


def read_row(out_dir, name, meta):
    match = LABEL_RE.match(name)
    if not match:
        return None
    label = name[:-len(".json")]
    with open(os.path.join(out_dir, name), encoding="utf-8") as handle:
        data = json.load(handle)
    summary = data.get("summary") or {}
    percentiles = data.get("latencyPercentiles") or {}
    codes = data.get("statusCodeDistribution") or {}
    errors = data.get("errorDistribution") or {}
    extra = meta.get(label, {})

    def millis(key):
        value = percentiles.get(key)
        return None if value is None else float(value) * 1000.0

    return {
        "label": label,
        "server": match.group("server"),
        "size_bytes": int(match.group("size")),
        "conns": int(match.group("conn")),
        "keepalive": "no" if match.group("mode") else "yes",
        "pass": int(match.group("pass")),
        "rps": float(summary.get("requestsPerSec") or 0.0),
        "successes": sum(int(v) for v in codes.values()),
        "success_rate": summary.get("successRate"),
        "duration_s": round(float(summary.get("total") or 0.0), 2),
        "p50_ms": millis("p50"),
        "p90_ms": millis("p90"),
        "p99_ms": millis("p99"),
        "mean_ms": (float(summary["average"]) * 1000.0
                    if summary.get("average") is not None else None),
        "mib_s": float(summary.get("sizePerSec") or 0.0) / 1048576.0,
        "deadline_aborts": sum(int(v) for v in errors.values()),
        "load_pct": extra.get("load_pct", ""),
        "contended": extra.get("contended", ""),
    }


def collect(out_dir):
    meta = read_meta(out_dir)
    rows = []
    for name in sorted(os.listdir(out_dir)):
        if not name.endswith(".json") or name.startswith("smoke"):
            continue
        row = read_row(out_dir, name, meta)
        if row:
            rows.append(row)
    return rows


def aggregate(rows):
    """One entry per (server, size, conns, keepalive), over its passes."""
    groups = {}
    for row in rows:
        key = (row["server"], row["size_bytes"], row["conns"], row["keepalive"])
        groups.setdefault(key, []).append(row)
    table = []
    for key, passes in groups.items():
        rps = sorted(p["rps"] for p in passes)
        mid = len(rps) // 2
        med = rps[mid] if len(rps) % 2 else (rps[mid - 1] + rps[mid]) / 2.0

        def mean(field):
            values = [p[field] for p in passes if p[field] is not None]
            return sum(values) / len(values) if values else None

        rates = [float(p["success_rate"]) for p in passes
                 if p["success_rate"] is not None]
        loads = [int(p["load_pct"]) for p in passes
                 if str(p["load_pct"]).isdigit()]
        table.append({
            "server": key[0], "size_bytes": key[1], "conns": key[2],
            "keepalive": key[3], "passes": len(passes),
            "rps_median": med,
            "rps_spread_pct": ((max(rps) - min(rps)) / med * 100.0
                               if med else 0.0),
            "p50_ms": mean("p50_ms"), "p90_ms": mean("p90_ms"),
            "p99_ms": mean("p99_ms"), "mean_ms": mean("mean_ms"),
            "mib_s": mean("mib_s"),
            "successes": sum(p["successes"] for p in passes),
            "aborts": sum(p["deadline_aborts"] for p in passes),
            "min_success_rate": min(rates) if rates else None,
            "contended": any(str(p["contended"]) == "1" for p in passes),
            "max_load_pct": max(loads) if loads else 0,
        })
    return table


def fmt_ms(value):
    return "-" if value is None else "%.2f" % value


def row_keys(table):
    """Distinct (body, conns, keep-alive) triples, in table order. The aggregate
    has one entry per server and triple, so iterating the aggregates themselves
    to choose which block to print repeats each block once per server."""
    return sorted({(e["size_bytes"], e["conns"], e["keepalive"]) for e in table})


def main_table(table, present):
    by_key = {(e["server"], e["size_bytes"], e["conns"], e["keepalive"]): e
              for e in table}
    lines = ["| body | conns | keep-alive | server | RPS | p50 ms | p90 ms "
             "| p99 ms | MiB/s | ok | pass spread |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for size, conns, keepalive in row_keys(table):
        for server in SERVER_ORDER:
            if server not in present:
                continue
            entry = by_key.get((server, size, conns, keepalive))
            if not entry:
                continue
            lines.append(
                "| %d B | %d | %s | %s | %d | %s | %s | %s | %.1f | %d | %.1f%% |"
                % (size, conns, keepalive, SERVER_LABEL.get(server, server),
                   entry["rps_median"], fmt_ms(entry["p50_ms"]),
                   fmt_ms(entry["p90_ms"]), fmt_ms(entry["p99_ms"]),
                   entry["mib_s"] or 0.0, entry["successes"],
                   entry["rps_spread_pct"]))
    return "\n".join(lines) + "\n"


def ratio_table(table, present):
    """NLang throughput as a multiple of each reference server's."""
    refs = [name for name in BASELINE_ORDER if name in present]
    if not refs:
        return ""
    by_key = {(e["server"], e["size_bytes"], e["conns"], e["keepalive"]): e
              for e in table}
    lines = ["", "NLang RPS as a multiple of each reference (>1 = NLang faster)",
             "", "| body | conns | keep-alive | " + " | ".join(
                 SERVER_LABEL[name] for name in refs) + " |",
             "|---|---|---|" + "---|" * len(refs)]
    for size, conns, keepalive in row_keys(table):
        ours = by_key.get(("nlang", size, conns, keepalive))
        if not ours:
            continue
        cells = []
        for name in refs:
            other = by_key.get((name, size, conns, keepalive))
            base = other["rps_median"] if other else 0.0
            cells.append("%.2fx" % (ours["rps_median"] / base)
                         if base else "-")
        lines.append("| %d B | %d | %s | %s |"
                     % (size, conns, keepalive, " | ".join(cells)))
    return "\n".join(lines) + "\n"


def flag_notes(table):
    # oha aborts whatever request each of its connections still has in flight
    # when the run's duration ends, so the expected tail is at most one abort per
    # connection per measured run: usually exactly that many, fewer on rows whose
    # connections were already idle when the deadline hit (verified on the
    # 2026-11-10 baseline's 120 runs: 81 had exactly that many, 39 had fewer, none
    # had more, and all 120 reported successRate 1.0). Only aborts above that
    # ceiling, a success rate below 1.0, or a contended row need the reader's eye.
    flags = [e for e in table if e["contended"] or (
        e["min_success_rate"] is not None and e["min_success_rate"] < 1.0)
        or e["aborts"] > e["conns"] * e["passes"]]
    if not flags:
        return "flags: none\n"
    return "flagged rows:\n" + "".join(
        "  %s %dB c%d ka=%s: contended=%s min_success_rate=%s"
        " deadline_aborts=%d (expected <= %d) max_load=%d%%\n"
        % (e["server"], e["size_bytes"], e["conns"], e["keepalive"],
           e["contended"], e["min_success_rate"], e["aborts"],
           e["conns"] * e["passes"], e["max_load_pct"]) for e in flags)


def main():
    out_dir = (sys.argv[1] if len(sys.argv) > 1
               else os.path.join(REPO_ROOT, "temp", "http_perf"))
    rows = collect(out_dir)
    if not rows:
        sys.stdout.write("no result files in %s\n" % out_dir)
        return 1
    table = aggregate(rows)
    present = {e["server"] for e in table}

    csv_path = os.path.join(out_dir, "results.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    versions_path = os.path.join(out_dir, "versions.txt")
    versions = ""
    if os.path.exists(versions_path):
        with open(versions_path, encoding="utf-8") as handle:
            versions = handle.read().strip() + "\n"
    text = (versions + "\n" + main_table(table, present)
            + ratio_table(table, present) + "\n" + flag_notes(table))
    with open(os.path.join(out_dir, "results.md"), "w", encoding="utf-8") as f:
        f.write(text)
    sys.stdout.write(text)
    sys.stdout.write("\nrows: %d, aggregates: %d, csv: %s\n"
                     % (len(rows), len(table), csv_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
