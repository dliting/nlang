# e2e_shard_test.py -- self-test corpus for the manifest sharder.
# Pins the invariants the tiered gates depend on: shard assignment is
# deterministic, every manifest data row lands in exactly one shard
# (coverage without loss or duplication), row text travels verbatim
# (expectations carry spaces), and the A/B comparator judges verdict-set
# equality rather than line order. Run: python e2e_shard_test.py
# Verdict line: "GATE e2e_shard_selftest: PASS" (or FAIL), exit code
# follows. Per the ctest-override lesson the sentinel is computed from
# the assertions themselves, never printed unconditionally.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from e2e_shard import (parse_manifest, shard_rows, write_shards,
                       parse_runner_output, ab_compare)


failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)

MANIFEST = """#section header comment
return_literal 42

var_decl_init 10
# another comment
compound_subscript_struct_elem_error compile_error compound assignment is not supported on struct values
stabilize_indep_sweep 42
stdlib_regex_size 0
conc_mutex_counter 16
"""


def test_parse_manifest():
    rows = parse_manifest(MANIFEST)
    check(len(rows) == 6, "parse: expected 6 data rows, got %d" % len(rows))
    check(rows[2] == "compound_subscript_struct_elem_error compile_error "
                     "compound assignment is not supported on struct values",
          "parse: row text must travel verbatim (expectation spaces), got %r"
          % rows[2])
    check(not any(r.lstrip().startswith("#") for r in rows),
          "parse: comment lines must be dropped")


def test_shard_rows_round_robin():
    rows = ["a 1", "b 2", "c 3", "d 4", "e 5", "f 6", "g 7"]
    shards = shard_rows(rows, 3)
    check(len(shards) == 3, "shard: expected 3 shards, got %d" % len(shards))
    check([len(s) for s in shards] == [3, 2, 2],
          "shard: round-robin sizes must be [3, 2, 2], got %r"
          % [len(s) for s in shards])
    check(shards[0] == ["a 1", "d 4", "g 7"],
          "shard: shard 0 must hold rows 0/3/6 in order, got %r" % shards[0])
    union = [r for s in shards for r in s]
    check(sorted(union) == sorted(rows),
          "shard: shards must cover every row exactly once")


def test_shard_rows_deterministic_and_clamped():
    rows = ["a 1", "b 2"]
    check(shard_rows(rows, 4) == shard_rows(rows, 4),
          "shard: assignment must be deterministic")
    clamped = shard_rows(rows, 4)
    check(len([s for s in clamped if s]) == 2,
          "shard: k above row count must clamp to one row per shard")


def test_parse_runner_output():
    log = """PASS return_literal (exit=42)
PASS var_decl_init (exit=10)

Failures:
  stabilize_indep_sweep: expected=42 actual=1
  stdlib_regex_size: runtime error: timed out
"""
    passed, failed = parse_runner_output(log)
    check(passed == {"return_literal", "var_decl_init"},
          "verdicts: passed set wrong, got %r" % passed)
    check(failed == {"stabilize_indep_sweep", "stdlib_regex_size"},
          "verdicts: failed set wrong, got %r" % failed)


def test_ab_compare():
    p1, f1 = {"a"}, set()
    p2, f2 = {"a"}, set()
    match, diffs = ab_compare(p1, f1, p2, f2)
    check(match and not diffs, "ab: identical sets must match")
    match, diffs = ab_compare(p1, {"b"}, p2, f2)
    check(not match and "b" in "".join(diffs),
          "ab: a serial-only failure must be reported")


def test_write_shards_round_trip():
    import tempfile
    rows = parse_manifest(MANIFEST)
    with tempfile.TemporaryDirectory() as tmp:
        paths = write_shards(rows, 2, Path(tmp))
        check(len(paths) == 2, "write: expected 2 shard files")
        merged = []
        for p in paths:
            merged.extend(parse_manifest(p.read_text(encoding="utf-8")))
        check(sorted(merged) == sorted(rows),
              "write: shard files must round-trip every row verbatim")


def main():
    test_parse_manifest()
    test_shard_rows_round_robin()
    test_shard_rows_deterministic_and_clamped()
    test_write_shards_round_trip()
    test_parse_runner_output()
    test_ab_compare()
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE e2e_shard_selftest: FAIL (%d)" % len(failures))
        return 1
    print("GATE e2e_shard_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
