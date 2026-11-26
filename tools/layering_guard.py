#!/usr/bin/env python3
"""Layering guard for the library DAG.

Checks the include/CMake face of the nlang_runtime <- nlang_core <-
{nlang_compiler, nlang_vm} layering described in
docs/dev/library_layering_design.md section 6. A default run scans the
repository, prints one `ASSERT<n> <file>:<line> <snippet>` line per
violation and exits 1 (0 when the tree is clean). `--selftest` replays the
fixture pairs under tests/fixtures/layering instead: every violation
fixture must trip exactly its own assertion and every clean fixture must
stay silent. Stdlib only.
"""

import argparse
import re
import sys
from pathlib import Path

# Spec 6: include-face assertions anchor on #include directive lines only;
# library names in comments and string literals do not count as hits.
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")

# Include-face assertions: (number, ((dir, filename patterns), ...), forbidden
# substrings). Quoted and angle-bracket spellings are listed apart because
# the include spelling itself carries the layering claim; the bare-name
# patterns in assertion 2 catch vm private headers pulled in unqualified.
SOURCE_ASSERTIONS = [
    (1, [("src/vm", ("*.h", "*.cpp"))],
     ('"nlang/compiler/', "<nlang/compiler/", '"builder/', '"codegen/')),
    (2, [("src/core", ("*.h", "*.cpp")),
         ("include/nlang/core", ("*.h",))],
     ('"nlang/compiler/', "<nlang/compiler/", '"builder/',
      '"nlang/vm/', "<nlang/vm/", '"VmExecutor', '"VmScheduler')),
    (3, [("src/compiler", ("*.h", "*.cpp"))],
     ('"nlang/vm/', "<nlang/vm/")),
]

# CMake-face assertions: (number, ((CMakeLists path, forbidden substrings),)).
# Substrings match the target names and borrowed source dirs that would let a
# forbidden dependency back in through the build graph.
CMAKE_ASSERTIONS = [
    (4, [("src/vm/CMakeLists.txt",
          ("nlang_compiler", "${PROJECT_SOURCE_DIR}/src/compiler")),
         ("src/compiler/CMakeLists.txt",
          ("nlang_vm", "${PROJECT_SOURCE_DIR}/src/vm"))]),
    (5, [("src/tools/nvm/CMakeLists.txt", ("nlang_compiler",)),
         ("src/tools/ndisasm/CMakeLists.txt", ("nlang_compiler",))]),
]

# Fixture pairs under tests/fixtures/layering: (assertion, directory name).
# Each pair holds a violation/ and a clean/ mirror of its assertion's scan
# surface, so a selftest run points the scanner at the fixture root.
FIXTURE_PAIRS = (
    (1, "a1_src_vm"), (2, "a2_src_core"), (2, "a2_include_core"),
    (3, "a3_src_compiler"), (4, "a4_src_vm"), (4, "a4_src_compiler"),
    (5, "a5_nvm"), (5, "a5_ndisasm"),
)


def _hit_lines(num, rel, path, patterns, include_only):
    """Lines of path containing any forbidden pattern, guard-formatted.

    include_only skips every line that is not an #include directive (spec 6
    anchoring); the CMake face passes False, where a bare mention of the
    forbidden target name is itself the risk, comments included.
    """
    hits = []
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        if include_only and not INCLUDE_DIRECTIVE.match(line):
            continue
        for pattern in patterns:
            if pattern in line:
                hits.append(f"ASSERT{num} {rel}:{n} {line.strip()}")
                break
    return hits


def scan(root):
    """All guard hits over root; missing dirs and files are skipped."""
    hits = []
    for num, dirs, patterns in SOURCE_ASSERTIONS:
        for dirname, globs in dirs:
            base = root / dirname
            if not base.is_dir():
                continue
            files = {f for g in globs for f in base.rglob(g) if f.is_file()}
            for f in sorted(files):
                hits += _hit_lines(num, f.relative_to(root).as_posix(), f,
                                   patterns, include_only=True)
    for num, targets in CMAKE_ASSERTIONS:
        for rel, patterns in targets:
            path = root / rel
            if path.is_file():
                hits += _hit_lines(num, rel, path, patterns, include_only=False)
    return hits


def selftest():
    """Judge the fixture pairs; 0 iff every pair behaves as labelled."""
    fixtures = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "layering"
    failures = []
    for num, name in FIXTURE_PAIRS:
        tag = f"ASSERT{num} "
        for kind, must_trip in (("violation", True), ("clean", False)):
            root = fixtures / name / kind
            if not root.is_dir():
                failures.append(f"{name}/{kind}: fixture dir missing")
                continue
            hits = scan(root)
            mine = [h for h in hits if h.startswith(tag)]
            other = [h for h in hits if not h.startswith(tag)]
            if must_trip:
                if not mine:
                    failures.append(f"{name}/{kind}: assertion {num} not tripped")
                if other:
                    failures.append(f"{name}/{kind}: tripped the wrong guard: {other[0]}")
            elif hits:
                failures.append(f"{name}/{kind}: expected clean, got {hits[0]}")
    total = 2 * len(FIXTURE_PAIRS)
    if failures:
        for failure in failures:
            print(f"SELFTEST FAIL {failure}")
        print(f"layering_guard selftest: {len(failures)}/{total} fixture checks failed")
        return 1
    print(f"layering_guard selftest: {total} fixture checks OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Guard the nlang library layering (include/CMake face).")
    parser.add_argument("--selftest", action="store_true",
                        help="judge the fixture pairs instead of the repository")
    parser.add_argument("--repo-root", type=Path, default=None,
                        help="repository root to scan (default: this script's repo)")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    root = (args.repo_root or Path(__file__).resolve().parent.parent).resolve()
    if not (root / "src").is_dir():
        print(f"layering_guard: {root} has no src/ directory")
        return 2
    hits = scan(root)
    for hit in hits:
        print(hit)
    print(f"layering_guard: {len(hits)} violation(s)")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
