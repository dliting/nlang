# test_impact_test.py -- self-test corpus for the T0 impact mapper.
# Pins the conservative mapping contract (gate_tiering_design.md):
# narrow high-confidence src leaves map to families, wide surfaces
# (compiler frontend) stay UNMAPPED so T0 falls back to full Debug
# units, tests/ changes select their own test, ignore globs silence
# no-impact paths, and merged families produce duplicate-free combined
# selectors. Run: python test_impact_test.py
# Verdict line: "GATE test_impact_selftest: PASS" (or FAIL), exit
# code follows.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_impact import compute_impact


failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)

FAMILIES = """#family <name> rule <arg>
family serialization src src/vm/IntrinsicsByteStream.cpp
family serialization unit bs_|serial|ncu_package
family serialization e2e ^(bs_|file_)
family concurrency src src/vm/VmExecutorConc*.cpp
family concurrency unit thread|conc|domainlock
family concurrency e2e ^(conc_|carrier)
ignore docs/**
"""


def test_leaf_file_maps_to_family():
    impact = compute_impact(
        ["src/vm/IntrinsicsByteStream.cpp"], FAMILIES)
    check(impact.unmapped == [],
          "impact: mapped leaf must not be unmapped, got %r"
          % impact.unmapped)
    check("bs_|serial|ncu_package" == impact.unit_regex,
          "impact: unit selector wrong, got %r" % impact.unit_regex)
    check("^(bs_|file_)" == impact.e2e_regex,
          "impact: e2e selector wrong, got %r" % impact.e2e_regex)


def test_unknown_src_is_unmapped():
    impact = compute_impact(["src/foo/Whatever.cpp"], FAMILIES)
    check(impact.unmapped == ["src/foo/Whatever.cpp"],
          "impact: unknown src must be unmapped, got %r" % impact.unmapped)


def test_tests_file_selects_own_test():
    impact = compute_impact(
        ["tests/test_vm/test_stabilize_copy.cpp"], FAMILIES)
    check(impact.unmapped == [], "impact: tests/ file must map")
    check(impact.unit_regex == "test_stabilize_copy",
          "impact: tests/ file must select its own test, got %r"
          % impact.unit_regex)


def test_families_merge_without_duplicates():
    impact = compute_impact(
        ["src/vm/IntrinsicsByteStream.cpp",
         "src/vm/VmExecutorConcPoll.cpp"], FAMILIES)
    check(impact.unit_regex.count("conc") == 1,
          "impact: merged unit regex must dedupe, got %r"
          % impact.unit_regex)


def test_ignore_glob_silences_path():
    impact = compute_impact(["docs/user_manual/zh/x.md"], FAMILIES)
    check(impact.unmapped == [],
          "impact: ignored path must not be unmapped")
    check(impact.unit_regex == "" and impact.e2e_regex == "",
          "impact: ignored path must contribute nothing")


def test_family_without_unit_rule_is_unmapped():
    broken = FAMILIES + "family ghost src src/vm/Ghost.cpp\n"
    impact = compute_impact(["src/vm/Ghost.cpp"], broken)
    check(impact.unmapped == ["src/vm/Ghost.cpp"],
          "impact: a src-matched family with no unit rule is a data "
          "bug and must fall back, got %r" % impact.unmapped)


def test_real_families_map_tensor_to_mnist_entries():
    # D-7 backfill: the tensor family in the real tools/test_families.txt
    # must map the native tensor sources to both mnist ctest entries, so
    # a tensor change backfills the 演示验收 rows that the slow-set -E
    # excludes from every scheduled round (甲 path).
    families = (Path(__file__).resolve().parent
                / "test_families.txt").read_text(encoding="utf-8")
    impact = compute_impact(["src/native/tensor/TensorCore.cpp"],
                            families)
    check(impact.unmapped == [],
          "impact: src/native/tensor/** must map to the tensor family, "
          "got %r" % (impact.unmapped,))
    check("native_tensor" in impact.unit_regex
          and "mnist_mlp" in impact.unit_regex,
          "impact: the tensor unit rule must carry both entry "
          "fragments, got %r" % impact.unit_regex)


def main():
    test_leaf_file_maps_to_family()
    test_unknown_src_is_unmapped()
    test_tests_file_selects_own_test()
    test_families_merge_without_duplicates()
    test_ignore_glob_silences_path()
    test_family_without_unit_rule_is_unmapped()
    test_real_families_map_tensor_to_mnist_entries()
    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        print("GATE test_impact_selftest: FAIL (%d)" % len(failures))
        return 1
    print("GATE test_impact_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
