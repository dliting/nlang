# tier_coverage_check.py -- the coverage-invariant guard for the
# tiered gates (docs/dev/gate_tiering_design.md): every e2e manifest
# entry must be selected by at least one tier's selector. Tiering
# reschedules WHEN a test runs; it must never create a test with no
# tier that runs it. A tier selector matching zero manifest rows is a
# misconfiguration (stale regex after a manifest rename) and fails
# loud too.
#
# Tier definitions live in tools/gate_tiers.txt (one source of truth,
# consumed by this guard and referenced by tools/gate_tier.sh):
#   # comment
#   tier <leg-name> selector <regex over manifest test names>
#
# Usage: python tier_coverage_check.py <manifest> <tiers-file>
# Verdict line: "GATE tier_coverage_check: PASS" (or FAIL), exit code
# follows.

import re
import sys
from pathlib import Path


def _manifest_names(manifest_text):
    names = []
    for line in manifest_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        names.append(stripped.split()[0])
    return names


def _tiers(tiers_text):
    tiers = []
    for line in tiers_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split(None, 3)
        if len(parts) != 4 or parts[0] != "tier" or parts[2] != "selector":
            raise ValueError("malformed tier line: %r" % line)
        tiers.append((parts[1], re.compile(parts[3])))
    return tiers


def check_coverage(manifest_text, tiers_text):
    """Returns (ok, uncovered_names, dead_selector_descriptions)."""
    names = _manifest_names(manifest_text)
    tiers = _tiers(tiers_text)
    uncovered = [n for n in names
                 if not any(rx.search(n) for _, rx in tiers)]
    dead = []
    for tier_name, rx in tiers:
        if not any(rx.search(n) for n in names):
            dead.append("%s: selector %r matches no manifest row"
                        % (tier_name, rx.pattern))
    return (not uncovered and not dead), uncovered, dead


def main():
    if len(sys.argv) != 3:
        print("usage: tier_coverage_check.py <manifest> <tiers-file>")
        return 2
    manifest_text = Path(sys.argv[1]).read_text(encoding="utf-8")
    tiers_text = Path(sys.argv[2]).read_text(encoding="utf-8")
    ok, uncovered, dead = check_coverage(manifest_text, tiers_text)
    for name in uncovered:
        print("UNCOVERED: %s" % name)
    for d in dead:
        print("DEAD SELECTOR: %s" % d)
    if not ok:
        print("GATE tier_coverage_check: FAIL (%d uncovered, %d dead)"
              % (len(uncovered), len(dead)))
        return 1
    print("GATE tier_coverage_check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
