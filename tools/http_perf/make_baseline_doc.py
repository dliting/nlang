"""Regenerate the http performance snapshot from the rig's measured tables.

The tracked document docs/dev/http_perf_baseline.md is prose plus two tables; the
prose lives in tools/http_perf/doc_template.md and the tables come straight from
the rig's own summary, so the published numbers can never drift from the run they
claim to report. Typing 60 rows by hand invites transcription errors.

    D:/dev/miniconda3/envs/py313/python.exe -I tools/http_perf/make_baseline_doc.py [template] [results.md] [out.md]

Without arguments it uses the repository's own locations: the template next to
this script, the summary in temp/http_perf/, and the tracked document under
docs/dev/. Re-running after a fresh measurement is the documented refresh path
(document section 6).
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_TEMPLATE = os.path.join(REPO_ROOT, "tools", "http_perf", "doc_template.md")
DEFAULT_RESULTS = os.path.join(REPO_ROOT, "temp", "http_perf", "results.md")
DEFAULT_OUT = os.path.join(REPO_ROOT, "docs", "dev", "http_perf_baseline.md")

# results.md spells servers the way the rig names them; the document uses the
# shorter labels, and both must mean the same row.
SERVER = {
    "NLang http.n (4 carriers)": "NLang http.n（4载体）",
    "NLang http.n (1 carrier)": "NLang http.n（1载体）",
    "Node http": "Node http",
    "nginx static (sendfile off)": "nginx静态",
    "nginx static (sendfile on)": "nginx静态（sendfile on）",
    "Go net/http": "Go net/http",
    "Rust axum/tokio": "Rust axum/tokio",
}
BODY = {"0 B": "0 B", "1024 B": "1 KiB", "8192 B": "8 KiB"}
KA = {"yes": "是", "no": "否"}

MAIN_HEAD = ("| 响应体 | 连接数 | 长连接 | 服务器 | RPS | P50 ms | P90 ms | P99 ms "
             "| MiB/s | 成功请求 | 两趟散布 |")
MAIN_SEP = "|" + "---|" * 11
RATIO_HEAD_NAMES = ["nginx静态", "Node http", "Go net/http", "Rust axum/tokio"]


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def blocks(text):
    """Yield each run of consecutive table lines, in document order."""
    block, out = [], []
    for line in text.splitlines():
        if line.startswith("|"):
            block.append(line)
        elif block:
            out.append(block)
            block = []
    if block:
        out.append(block)
    return out


def main_table(lines):
    out = [MAIN_HEAD, MAIN_SEP]
    for line in lines[2:]:
        c = cells(line)
        if len(c) != 11:
            raise SystemExit("unexpected main-table column count: %r" % line)
        server = SERVER.get(c[3])
        if server is None:
            raise SystemExit("unmapped server label: %r" % c[3])
        body, conns, ka = BODY.get(c[0]), c[1], KA.get(c[2])
        if body is None or ka is None:
            raise SystemExit("unmapped row label: %r" % line)
        out.append("| %s | %s | %s | %s | %s |" % (
            body, conns, ka, server, " | ".join(c[4:])))
    return out


def ratio_table(lines):
    head = cells(lines[0])
    names = [SERVER.get(n) or n for n in head[3:]]
    if names != RATIO_HEAD_NAMES:
        raise SystemExit("ratio table columns changed: %r" % names)
    out = ["| 响应体 | 连接数 | 长连接 | " + " | ".join(names) + " |",
           "|" + "---|" * 7]
    for line in lines[2:]:
        c = cells(line)
        if len(c) != 7:
            raise SystemExit("unexpected ratio column count: %r" % line)
        out.append("| %s | %s | %s | %s |" % (
            BODY[c[0]], c[1], KA[c[2]], " | ".join(c[3:])))
    return out


def main():
    args = sys.argv[1:]
    template_path = args[0] if len(args) > 0 else DEFAULT_TEMPLATE
    results_path = args[1] if len(args) > 1 else DEFAULT_RESULTS
    out_path = args[2] if len(args) > 2 else DEFAULT_OUT
    with open(template_path, encoding="utf-8") as fh:
        template = fh.read()
    with open(results_path, encoding="utf-8") as fh:
        results = fh.read()
    tables = blocks(results)
    if len(tables) != 2:
        raise SystemExit("expected 2 tables in results.md, found %d" % len(tables))
    main_rows, ratio_rows = main_table(tables[0]), ratio_table(tables[1])
    # A row missing its closing pipe still renders as a table in most viewers and
    # silently drops a column, so the pipe count is checked, not eyeballed.
    for rows, pipes, what in ((main_rows, 12, "main"), (ratio_rows, 8, "ratio")):
        for row in rows:
            if row.count("|") != pipes:
                raise SystemExit("%s table row has %d pipes, want %d: %r" % (
                    what, row.count("|"), pipes, row))
    doc = template.replace("{{MAIN_TABLE}}", "\n".join(main_rows))
    doc = doc.replace("{{RATIO_TABLE}}", "\n".join(ratio_rows))
    if "{{" in doc:
        raise SystemExit("unfilled placeholder left in the document")
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(doc)
    print("main rows %d, ratio rows %d -> %s" % (
        len(main_rows) - 2, len(ratio_rows) - 2, out_path))


if __name__ == "__main__":
    main()
