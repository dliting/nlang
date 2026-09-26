"""check_source_size.py -- repository source-size guard.

Rules (spec 2026-09-25 maintainability refactor):
  * every scanned file is <= 500 lines;
  * every function definition is <= 50 lines, where a definition starts
    at a column-0 signature line and ends at the next column-0 "}".

Excluded from scanning: src/compiler/generated/, src/3rdparty/.
Allowlist (allowlist.json, same directory): entries
  {"path": "src/vm/VmExecutor.cpp", "function": "ExecuteFunction", "reason": "..."}
skips one function's span rule, or without "function" skips the whole
file's both rules. Every entry MUST carry a non-empty reason.

Detection heuristics (documented limits): signatures that end with
"= default;" / "= delete;" / ";" before any "{" are skipped; one-line
definitions are treated as zero-length. Column-0 style is the repo's
prevailing layout; deviations miscount in BOTH directions -- indented
definitions are invisible to the function rule (undercount), while
unbraced col-0 constructs can swallow text up to the next col-0 "}"
(overcount), and a signature whose "{" sits more than 4 lines down is
measured from the 4th line (short by the difference). The allowlist is
the escape hatch for both directions, and reasons keep it honest.

Overloads: spans are yielded in order of appearance, so each overload
of a repeated name (e.g. the Access family) is measured separately; an
allowlist entry exempts every same-name function in that one file.

Exit 0 = clean; exit 1 = violations (listed); exit 2 = usage errors.
"""
import json
import re
import sys
from pathlib import Path

FILE_MAX_LINES = 500
FUNC_MAX_LINES = 50
SKIP_DIRS = ("generated", "3rdparty")
KEYWORDS = ("if", "for", "while", "switch", "return", "else", "namespace",
            "class", "struct", "enum", "using", "throw", "new", "delete",
            "do", "try", "catch", "extern", "template", "public", "private",
            "protected", "operator")
SIG_RE = re.compile(
    r"^(?:static\s+|constexpr\s+|inline\s+|virtual\s+|explicit\s+)*"
    r"(?:[A-Za-z_][A-Za-z0-9_:,<>\s&*]*?\s+)?"
    r"((?:[A-Za-z_][A-Za-z0-9_]*::)+~?[A-Za-z_][A-Za-z0-9_]*"
    r"|[A-Za-z_][A-Za-z0-9_]*)\s*\(")


def scanned_files(repo: Path):
    for sub in ("src", "include"):
        for p in sorted((repo / sub).rglob("*.[ch]pp")) \
               + sorted((repo / sub).rglob("*.h")):
            parts = p.relative_to(repo).parts
            if any(d in SKIP_DIRS for d in parts):
                continue
            yield p


def function_spans(lines):
    """Yields (name, start_line_1based, length) for column-0 definitions."""
    i = 0
    in_block_comment = False
    while i < len(lines):
        line = lines[i]
        # Skip block-comment regions: prose like "position (pinned)" in a
        # header comment matches the signature regex and creates phantom
        # functions. Enter on "/*" without a same-line close, exit on "*/".
        if in_block_comment:
            if "*/" in line:
                in_block_comment = False
            i += 1
            continue
        if line.lstrip().startswith("/*"):
            if "*/" not in line:
                in_block_comment = True
            i += 1
            continue
        m = SIG_RE.match(line)
        if m and not line.lstrip().startswith(tuple(k + " " for k in KEYWORDS)) \
           and not line.startswith("//") \
           and not m.group(1).isupper():  # ALL_CAPS = X-macro invocation, not a function
            name = m.group(1)
            # Skip declarations and defaulted/deleted definitions: find the
            # first of ";" or "{" within the next 4 lines.
            head = line
            j = i
            while "{" not in head and j < min(i + 4, len(lines)) - 1:
                j += 1
                head += lines[j]
            if ";" in head and "{" not in head.split(";")[0]:
                i += 1
                continue
            # Span ends at the next column-0 closing brace.
            k = j
            while k < len(lines) and not lines[k].startswith("}"):
                k += 1
            yield (name, i + 1, (k - i + 1) if k < len(lines) else 0)
            i = k + 1 if k < len(lines) else i + 1
        else:
            i += 1


def main():
    here = Path(__file__).resolve().parent
    repo = here.parent.parent
    allow = json.loads((here / "allowlist.json").read_text(encoding="utf-8"))
    by_file = {}
    for e in allow:
        assert e.get("reason", "").strip(), f"allowlist entry without reason: {e}"
        by_file.setdefault(e["path"], []).append(e)

    violations = []
    for p in scanned_files(repo):
        rel = p.relative_to(repo).as_posix()
        entries = by_file.get(rel, [])
        file_exempt = any("function" not in e for e in entries)
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        if not file_exempt and len(lines) > FILE_MAX_LINES:
            violations.append(f"{rel}: file has {len(lines)} lines (> {FILE_MAX_LINES})")
        if file_exempt:
            continue
        exempt_funcs = {e["function"] for e in entries if "function" in e}
        for name, start, length in function_spans(lines):
            if name in exempt_funcs or f"{rel}::{name}" in exempt_funcs:
                continue
            if length > FUNC_MAX_LINES:
                violations.append(
                    f"{rel}:{start}: function {name} spans {length} lines (> {FUNC_MAX_LINES})")

    if violations:
        print(f"source-size guard: {len(violations)} violation(s)")
        for v in violations:
            print("  " + v)
        sys.exit(1)
    print("source-size guard: clean")


if __name__ == "__main__":
    main()
