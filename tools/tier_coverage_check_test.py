# tier_coverage_check_test.py -- self-test corpus for the tier
# coverage guard. Pins the coverage invariant (gate_tiering_design.md):
# every e2e manifest entry is selected by at least one tier's selector;
# a tier selector that matches nothing is a misconfiguration and fails
# loud; comment and blank lines never count. Run:
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


def test_deferred_names_exempt_from_coverage():
    #The temporary perf trim cuts listed names from every tier by user
    #ruling; the guard must not report them as uncovered (the cut is
    #intentional, not a tiering gap). Dead selectors still fail.
    ok, uncovered, dead = check_coverage(
        MANIFEST, TIERS_GAP, frozenset({"return_literal",
                                        "stabilize_indep_sweep"}))
    check(ok, "coverage: deferred names must be exempt, uncovered=%r"
          % uncovered)
    check(not dead, "coverage: no dead selectors expected, got %r" % dead)


def test_undeferred_gap_still_fails():
    #Exemption applies only to listed names: an unlisted gap must keep
    #failing so the trim cannot mask a real tiering hole.
    ok, uncovered, dead = check_coverage(
        MANIFEST, TIERS_GAP, frozenset({"return_literal"}))
    check(not ok, "coverage: unlisted gap must still fail")
    check("stabilize_indep_sweep" in uncovered,
          "coverage: the unlisted name must be named, got %r" % uncovered)


def main():
    test_full_coverage()
    test_uncovered_row_fails()
    test_dead_selector_fails()
    test_deferred_names_exempt_from_coverage()
    test_undeferred_gap_still_fails()
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
