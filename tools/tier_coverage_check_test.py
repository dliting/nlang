# tier_coverage_check_test.py -- self-test corpus for the tier
# coverage guard. Pins the coverage invariant (gate_tiering_design.md):
# every e2e manifest entry is selected by at least one tier's selector;
# a tier selector that matches nothing is a misconfiguration and fails
# loud; comment and blank lines never count; a claim line validates its
# shape but never joins the coverage math. Run:
# python tier_coverage_check_test.py
# Verdict line: "GATE tier_coverage_check_selftest: PASS" (or FAIL),
# exit code follows.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tier_coverage_check import check_coverage


failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)

MANIFEST = """#header
return_literal 42
conc_mutex_counter 16
stabilize_indep_sweep 42
"""

TIERS_FULL = """# tier <name> selector <regex>
tier T1-debug-e2e selector .
tier T1-asan-subset selector ^(conc_|stabilize_)
"""

TIERS_GAP = """tier T1-only-conc selector ^(conc_)
"""

TIERS_EMPTY = """tier T1-ghost selector ^(never_matches_anything_)
"""

# The claim roster line (gate timing design D-11) names a named resource
# and the entries that hold it; the coverage guard validates its shape
# but never lets it participate in the coverage math.
TIERS_WITH_CLAIM = """tier T1-debug-e2e selector .
tier T1-asan-subset selector ^(conc_|stabilize_)

claim nide-gui mainwindow_tests editor_tests terminal_tests
"""


def _claim_verdict(tiers_text):
    """Returns the ValueError message a claim line must raise, or None
    when the tiers text parses."""
    try:
        check_coverage(MANIFEST, tiers_text)
    except ValueError as exc:
        return str(exc)
    return None


def test_full_coverage():
    ok, uncovered, dead = check_coverage(MANIFEST, TIERS_FULL)
    check(ok, "coverage: fully covered manifest must pass, uncovered=%r"
          % uncovered)
    check(not dead, "coverage: no dead selectors expected, got %r" % dead)


def test_uncovered_row_fails():
    ok, uncovered, dead = check_coverage(MANIFEST, TIERS_GAP)
    check(not ok, "coverage: an unselected row must fail the guard")
    check("return_literal" in uncovered and "stabilize_indep_sweep"
          in uncovered,
          "coverage: uncovered rows must be named, got %r" % uncovered)


def test_dead_selector_fails():
    ok, uncovered, dead = check_coverage(MANIFEST, TIERS_FULL
                                         + TIERS_EMPTY)
    check(not ok, "coverage: a selector matching zero rows must fail")
    check(any("T1-ghost" in d for d in dead),
          "coverage: the dead selector must be named, got %r" % dead)


def test_claim_line_does_not_affect_coverage():
    try:
        ok, uncovered, dead = check_coverage(MANIFEST, TIERS_WITH_CLAIM)
    except ValueError as exc:
        check(False, "coverage: a well-formed claim line must parse, "
                     "got %s" % exc)
        return
    check(ok, "coverage: a claim line must not change the verdict, "
              "uncovered=%r dead=%r" % (uncovered, dead))


def test_claim_line_without_entries_rejected():
    msg = _claim_verdict("claim nide-gui\n")
    check(msg is not None and "claim line" in msg,
          "claim shape: a claim without entries must raise a named "
          "malformed claim line, got %r" % msg)


def test_claim_line_bad_charset_rejected():
    msg = _claim_verdict("claim bad/name mainwindow_tests\n")
    check(msg is not None and "claim line" in msg,
          "claim shape: a resource name outside [A-Za-z0-9_.-] must "
          "raise a named malformed claim line, got %r" % msg)


def main():
    test_full_coverage()
    test_uncovered_row_fails()
    test_dead_selector_fails()
    test_claim_line_does_not_affect_coverage()
    test_claim_line_without_entries_rejected()
    test_claim_line_bad_charset_rejected()
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE tier_coverage_check_selftest: FAIL (%d)"
              % len(failures))
        return 1
    print("GATE tier_coverage_check_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
