# test_impact.py -- the T0 impact mapper for the tiered gates
# (docs/dev/gate_tiering_design.md). Maps changed source paths onto
# test families via tools/test_families.txt; the union of the matched
# families' selectors is the T0 leg. Conservatism rules:
#   - a changed path no family covers is UNMAPPED -> the caller falls
#     back to full Debug units (never "fewer tests");
#   - a src-matched family with no unit rule is a families-file data
#     bug and reports UNMAPPED too (loud, not silent);
#   - tests/** test files select their own test executable (the stem
#     IS the ctest entry name);
#   - ignore globs silence paths with no test impact (docs).
#
# Families file format (tools/test_families.txt):
#   family <name> src <glob>
#   family <name> unit <ctest -R regex fragment>
#   family <name> e2e <manifest-name regex fragment>
#   ignore <glob>
#
# Usage: python test_impact.py <families-file> <changed-path>...
# Output lines: "unit <regex>", "e2e <regex>", "UNMAPPED <path>".
# Exit 0 mapped, 3 unmapped present (caller falls back).

import fnmatch
import sys
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath


@dataclass
class Impact:
    unit_regex: str = ""
    e2e_regex: str = ""
    unmapped: list = field(default_factory=list)


def _norm(path):
    return PurePosixPath(path.replace("\\", "/")).as_posix()


def _parse_families(text):
    """Returns (src_rules, ignore_globs, unit_by_family, e2e_by_family).
    src_rules: [(glob, family-name)]."""
    src_rules = []
    ignore_globs = []
    unit_by_family = {}
    e2e_by_family = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        if tokens[0] == "ignore" and len(tokens) == 2:
            ignore_globs.append(_norm(tokens[1]))
        elif tokens[0] == "family" and len(tokens) == 4 \
                and tokens[2] in ("src", "unit", "e2e"):
            name, kind, arg = tokens[1], tokens[2], tokens[3]
            if kind == "src":
                src_rules.append((_norm(arg), name))
            elif kind == "unit":
                unit_by_family[name] = arg
            else:
                e2e_by_family[name] = arg
        else:
            raise ValueError("malformed families line: %r" % raw)
    return src_rules, ignore_globs, unit_by_family, e2e_by_family


def compute_impact(changed_paths, families_text):
    src_rules, ignore_globs, unit_by_family, e2e_by_family = \
        _parse_families(families_text)
    unit_parts = set()
    e2e_parts = set()
    matched_families = set()
    unmapped = []
    for raw_path in changed_paths:
        path = _norm(raw_path)
        if any(fnmatch.fnmatch(path, glob) for glob in ignore_globs):
            continue
        #tests/** test sources select their own executable: the stem
        #IS the ctest entry name.
        if path.startswith("tests/"):
            stem = Path(path).stem
            if stem.startswith("test_"):
                unit_parts.add(stem)
                continue
        families = {name for glob, name in src_rules
                    if fnmatch.fnmatch(path, glob)}
        if not families or \
                not any(name in unit_by_family for name in families):
            #No covering family, or a covering family that declares no
            #unit rule (families-file data bug): fall back conservatively.
            unmapped.append(raw_path)
            continue
        matched_families |= families
    for name in matched_families:
        unit_parts.add(unit_by_family[name])
        if name in e2e_by_family:
            e2e_parts.add(e2e_by_family[name])
    return Impact("|".join(sorted(unit_parts)),
                  "|".join(sorted(e2e_parts)), unmapped)


def main():
    if len(sys.argv) < 3:
        print("usage: test_impact.py <families-file> <changed-path>...")
        return 2
    impact = compute_impact(sys.argv[2:],
                            Path(sys.argv[1]).read_text(encoding="utf-8"))
    if impact.unit_regex:
        print("unit %s" % impact.unit_regex)
    if impact.e2e_regex:
        print("e2e %s" % impact.e2e_regex)
    for path in impact.unmapped:
        print("UNMAPPED %s" % path)
    if impact.unmapped:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
