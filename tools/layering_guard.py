#!/usr/bin/env python3
"""Layering guard for the library DAG.

Checks the include/CMake face of the nlang_runtime <- nlang_core <-
{nlang_compiler, nlang_vm} layering described in
docs/dev/library_layering_design.md section 6. The include face is
resolution-based: every #include directive line in a guarded tree is
resolved with the compiler's own search for this repo (a quoted name
beside its includer first, then the public include/ root; an angled
name under include/ only) and the RESOLVED file decides -- the
include's spelling (path-prefixed, bare, or relative) carries no
layering claim, the file it opens does. A substring face cannot make
that distinction: the 2026-11-26 review walked three holes in it (a
bare name the pattern table had not enumerated, a relative path that
no prefix matches, and the vm public header tree it never scanned),
and patching patterns per hole is how the holes accumulated.

An include that resolves to nothing is a hit too: in the guarded trees
every legal include resolves into an allowed zone, so an unresolved
one is either a missing file or one that reaches its target only
through a borrowed include directory -- which is what the CMake face
forbids. Angled non-nlang names are system headers and stay clean.

A default run scans the repository, prints one `ASSERT<n> <file>:<line>
<include> -> <target>` line per violation and exits 1 (0 when the tree
is clean). `--selftest` replays the fixture pairs under
tests/fixtures/layering instead: every violation fixture must trip
exactly its own assertion and every clean fixture must stay silent.
Stdlib only.
"""

import argparse
import re
import sys
from pathlib import Path

# Spec 6: include-face assertions anchor on #include directive lines only;
# library names in comments and string literals do not count as hits.
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")
INCLUDE_NAME = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# Allowed zones per guarded library, root-relative prefixes: the
# guarded library's own trees plus the public include/ faces of the
# libraries the DAG allows it to consume (runtime <- core <-
# {compiler, vm}; compiler PRIVATE langservice; vm's langservice edge
# was removed by the P3 ruling, so the vm face must not reach
# langservice in either form). Vendored third-party code sits below
# the whole DAG and is allowed for every library.
VM_ZONES = ("src/vm", "include/nlang/vm", "include/nlang/core",
            "include/nlang/runtime", "src/3rdparty")
CORE_ZONES = ("src/core", "include/nlang/core", "include/nlang/runtime",
              "src/3rdparty")
COMPILER_ZONES = ("src/compiler", "include/nlang/compiler",
                  "include/nlang/core", "include/nlang/runtime",
                  "include/nlang/langservice", "src/3rdparty")

# The private include dirs each target's CMake adds beyond the public
# include/ root (target_include_directories in each CMakeLists): the
# target's own source root and its builder/codegen sub-dirs let bare
# names reach anywhere in the same tree, the compiler also publishes
# generated/ and its own public header subdir (nlang.tab.h's prologue
# includes bare names), and the vm borrows the vendored wepoll dir.
# These mirror the compiler's real search so the walker resolves what
# the build resolves.
VM_PRIVATE_DIRS = ("src/vm", "src/3rdparty/wepoll")
CORE_PRIVATE_DIRS = ("src/core",)
COMPILER_PRIVATE_DIRS = ("src/compiler", "src/compiler/generated",
                         "src/compiler/builder", "src/compiler/codegen",
                         "include/nlang/compiler")

# Include-face assertions: (number, ((guarded dir, private dirs), ...)).
# One number per library face: the private tree and the public header
# tree share the assertion because they share the layering claim.
SOURCE_ASSERTIONS = [
    (1, [("src/vm", VM_PRIVATE_DIRS), ("include/nlang/vm", ())]),
    (2, [("src/core", CORE_PRIVATE_DIRS),
         ("include/nlang/core", ())]),
    (3, [("src/compiler", COMPILER_PRIVATE_DIRS)]),
]

ZONES_BY_ASSERTION = {1: VM_ZONES, 2: CORE_ZONES, 3: COMPILER_ZONES}

# CMake-face assertions: (number, ((CMakeLists path, forbidden
# substrings),)). Substrings match the target names and borrowed source
# dirs that would let a forbidden dependency back in through the build
# graph -- the face the resolution walker cannot see, because a
# borrowed include dir makes bare names resolve outside the repo's own
# conventions. Comments are stripped before matching: a comment cannot
# add a dependency, so a forbidden name visible only in one is
# documentation, not a hit. Every side is covered: vm and compiler
# must not name or borrow each other, core must not name or borrow
# either of them, and vm must not re-grow the langservice link the P3
# ruling removed.
CMAKE_ASSERTIONS = [
    (4, [("src/vm/CMakeLists.txt",
          ("nlang_compiler", "nlang_langservice",
           "${PROJECT_SOURCE_DIR}/src/compiler")),
         ("src/core/CMakeLists.txt",
          ("nlang_compiler", "nlang_vm",
           "${PROJECT_SOURCE_DIR}/src/compiler",
           "${PROJECT_SOURCE_DIR}/src/vm")),
         ("src/compiler/CMakeLists.txt",
          ("nlang_vm", "${PROJECT_SOURCE_DIR}/src/vm"))]),
    (5, [("src/tools/nvm/CMakeLists.txt", ("nlang_compiler",)),
         ("src/tools/ndb/CMakeLists.txt", ("nlang_compiler",)),
         ("src/tools/ndisasm/CMakeLists.txt", ("nlang_compiler",))]),
]

# Fixture pairs under tests/fixtures/layering: (assertion, directory name).
# Each pair holds a violation/ and a clean/ mirror of its assertion's scan
# surface, so a selftest run points the scanner at the fixture root.
FIXTURE_PAIRS = (
    (1, "a1_src_vm"), (1, "a1_include_vm"),
    (2, "a2_src_core"), (2, "a2_include_core"),
    (3, "a3_src_compiler"), (4, "a4_src_vm"), (4, "a4_src_compiler"),
    (4, "a4_src_core"), (5, "a5_nvm"), (5, "a5_ndisasm"), (5, "a5_ndb"),
)


def _resolve(root, includer, quote, name, private_dirs):
    """The file the compiler opens for this include, or None.

    A quoted name is searched beside its includer first, then the
    public include/ root, then the target's private include dirs --
    the search order this repo's conventions use (each CMakeLists
    declares them; gate_freshness's closure walker resolves the same
    way for the native tree).
    """
    if quote == '"':
        candidates = ([includer.parent / name, root / "include" / name]
                      + [root / one / name for one in private_dirs])
    else:
        candidates = [root / "include" / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _include_hits(num, rel, path, zones, private_dirs, root):
    """Guard-formatted hits for every out-of-zone include in path."""
    hits = []
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        if not INCLUDE_DIRECTIVE.match(line):
            continue
        match = INCLUDE_NAME.match(line)
        if not match:
            continue
        quote, name = match.groups()
        target = _resolve(root, path, quote, name, private_dirs)
        if target is None:
            if quote == "<" and not name.startswith("nlang/"):
                continue  # system header
            hits.append(
                f"ASSERT{num} {rel}:{n} {name} -> unresolved (beside "
                "the includer, under include/ and in this target's "
                "private dirs it does not exist; a borrowed include "
                "dir would be needed, which the CMake face forbids)")
            continue
        rel_target = target.resolve().relative_to(root.resolve()).as_posix()
        if not any(rel_target == zone or rel_target.startswith(zone + "/")
                   for zone in zones):
            hits.append(
                f"ASSERT{num} {rel}:{n} {name} -> {rel_target} "
                "(outside this tree's allowed zones)")
    return hits


def _cmake_code(line):
    """The code part of a CMakeLists line, with the comment cut off.

    Cut at the first # that is not inside double quotes -- CMake's own
    comment rule. A quoted # stays literal, so a forbidden name inside
    a string argument still matches: this face over-approximates by
    design, and a string carrying the target name is worth a human
    look even when it is not a link.
    """
    in_quotes = False
    for i, ch in enumerate(line):
        if ch == '"':
            in_quotes = not in_quotes
        elif ch == "#" and not in_quotes:
            return line[:i]
    return line


def _hit_lines(num, rel, path, patterns, include_only):
    """Lines of path containing any forbidden pattern, guard-formatted.

    include_only skips every line that is not an #include directive
    (spec 6 anchoring); the CMake face passes False and matches the
    comment-stripped code part of every line -- a comment cannot add
    a dependency, so a name visible only in one does not count.
    """
    hits = []
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        if include_only:
            if not INCLUDE_DIRECTIVE.match(line):
                continue
        else:
            line = _cmake_code(line)
        for pattern in patterns:
            if pattern in line:
                hits.append(f"ASSERT{num} {rel}:{n} {line.strip()}")
                break
    return hits


def scan(root):
    """All guard hits over root; missing dirs and files are skipped."""
    hits = []
    for num, dirs in SOURCE_ASSERTIONS:
        for dirname, private_dirs in dirs:
            zones = ZONES_BY_ASSERTION[num]
            base = root / dirname
            if not base.is_dir():
                continue
            files = {f for g in ("*.h", "*.cpp", "*.hpp")
                     for f in base.rglob(g) if f.is_file()}
            for f in sorted(files):
                hits += _include_hits(num, f.relative_to(root).as_posix(),
                                      f, zones, private_dirs, root)
    for num, targets in CMAKE_ASSERTIONS:
        for rel, patterns in targets:
            path = root / rel
            if path.is_file():
                hits += _hit_lines(num, rel, path, patterns,
                                   include_only=False)
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
