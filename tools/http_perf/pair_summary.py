#!/usr/bin/env python3
"""Summarize one probe_pair_ab.sh run and apply the pre-registered keep rule.

Usage: py313 -I tools/http_perf/pair_summary.py <out-root>

Reads <out-root>/p<i>/b|n/pace_q200.json for every pair index that exists and
reports the p50 medians (oha reports seconds; everything here is milliseconds)
plus the per-pair differences. Because the two arms alternate within a pair,
the differences cancel machine drift; the keep rule is stated before the run
so it cannot be argued into the data afterwards: keep the new side only if the
difference median exceeds the difference spread AND the gain is at least 1 %.
"""

import json
import os
import statistics
import sys

def p50_ms(path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return float(data["latencyPercentiles"]["p50"]) * 1000.0

def main():
    if len(sys.argv) != 2:
        print("usage: py313 -I tools/http_perf/pair_summary.py <out-root>")
        return 2
    root = sys.argv[1]
    pairs = []
    index = 1
    while True:
        b = os.path.join(root, "p%d" % index, "b", "pace_q200.json")
        n = os.path.join(root, "p%d" % index, "n", "pace_q200.json")
        if not (os.path.isfile(b) and os.path.isfile(n)):
            break
        pairs.append((p50_ms(b), p50_ms(n)))
        index += 1
    if not pairs:
        print("no complete pairs under %s/p<i>/b|n/pace_q200.json" % root)
        return 1

    for i, (b, n) in enumerate(pairs, 1):
        print("p%-3d base=%.3f  new=%.3f  diff=%.3f" % (i, b, n, b - n))
    diffs = [b - n for b, n in pairs]
    base_med = statistics.median([b for b, _ in pairs])
    new_med = statistics.median([n for _, n in pairs])
    diff_med = statistics.median(diffs)
    diff_spread = max(diffs) - min(diffs)
    gain = (base_med - new_med) / base_med * 100.0
    positive = sum(1 for d in diffs if d > 0)
    print("n=%d  base_med=%.3f  new_med=%.3f  diff_med=%.3f  diff_spread=%.3f"
          "  gain=%.2f%%  sign=%d/%d"
          % (len(pairs), base_med, new_med, diff_med, diff_spread, gain,
             positive, len(pairs)))
    keep = diff_med > diff_spread and gain >= 1.0
    print("rule(diff_med > diff_spread AND gain >= 1%%): %s"
          % ("KEEP" if keep else "STOP"))
    return 0

if __name__ == "__main__":
    sys.exit(main())
