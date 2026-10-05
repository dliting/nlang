"""Style guards for the user manual (docs/user_manual/{zh,en}).

Two checks share one markdown line scanner:

- compact spacing: an ASCII space at a CJK/fullwidth <-> Latin/digit
  boundary in prose (rule A of docs/user-manual-style-guide.md);
- abbreviation co-occurrence: when a controlled abbreviation occurs in
  body prose, a full form from the guide's term table must co-occur on
  the same page (headings keep the short form and are not scanned).

The term table is parsed from the style guide so it stays the single
source (same idea as verify_package.py parsing the .nmod version floor
from CompiledModule.h). Inline code, fenced code blocks, front-matter
and URLs are exempt in both scans; the guide itself lives outside the
scanned tree, so its specimens never self-flag.
"""

from __future__ import annotations

import re
from pathlib import Path

# Derive the repository root from this file's location so the guards
# work from any working directory (ctest invokes the docs pytest from
# the build tree, not the source root).
_REPO_ROOT = Path(__file__).resolve().parents[4]
GUIDE_PATH = _REPO_ROOT / "docs" / "user-manual-style-guide.md"
MANUAL_ROOT = _REPO_ROOT / "docs" / "user_manual"

# Product/tool names never treated as expandable abbreviations.
PROPER_NOUNS = {
    "NLANG", "QT", "PYTHON", "WINDOWS", "LINUX", "LLVM", "FLEX", "BISON",
    "MKDOCS", "MATERIAL", "NSIS", "CPACK", "UNICODE",
    # Product vocabulary: "VM" is NLang's own engine term (like ncc/nvm),
    # and VC/MSVC name Microsoft's toolchain -- none are expansion
    # targets. IEEE/POSIX name standards bodies, JS names JavaScript,
    # FNV names the Fowler-Noll-Vo hash -- conventional short names.
    "VM", "VC", "MSVC", "IEEE", "POSIX", "JS", "FNV",
}


def _strip_inline_code(line: str) -> str:
    return re.sub(r"`[^`]*`", "\x00", line)


def _strip_link_targets(line: str) -> str:
    return re.sub(r"\]\([^)]*\)", "](\x00)", line)


def prose_lines(text: str, *, drop_headings: bool):
    """Yield (lineno, line) for prose lines only.

    Skips front-matter and fenced code blocks; optionally skips
    headings (abbreviation policy: headings keep the short form).
    Inline code spans, link targets and bare URLs are blanked with
    \\x00 so they neither match nor glue false boundaries.
    """
    text = text.lstrip(chr(0xFEFF))
    in_fence = False
    in_front = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if lineno == 1 and stripped == "---":
            in_front = True
            continue
        if in_front:
            if stripped == "---":
                in_front = False
            continue
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if drop_headings and stripped.startswith("#"):
            continue
        line = _strip_link_targets(raw)
        line = re.sub(r"https?://\S+", "\x00", line)
        yield lineno, _strip_inline_code(line)


def parse_term_table(guide_text: str) -> dict:
    """Parse the guide's three-column abbreviation table.

    Rows look like '| PRNG | 伪随机数生成器 | pseudorandom number
    generator |'. The header row cannot match (its first column is
    not ASCII); the separator row matches the regex but is rejected
    by the letter-or-digit check below.
    """
    terms = {}
    for row in guide_text.splitlines():
        m = re.match(
            r"^\|\s*([A-Za-z0-9-]+)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*$",
            row,
        )
        if not m:
            continue
        abbr, zh_full, en_full = m.groups()
        if not re.search(r"[A-Za-z0-9]", abbr):
            continue  # separator row like |---|---|---|
        terms[abbr] = (zh_full, en_full)
    return terms


def load_terms() -> dict:
    return parse_term_table(GUIDE_PATH.read_text(encoding="utf-8"))


def _occurrences(abbr: str):
    return re.compile(
        rf"(?<![A-Za-z0-9]){re.escape(abbr)}(?![A-Za-z0-9])"
    )


def abbreviation_violations_in_text(text: str, terms: dict, lang: str):
    """Pages where an abbreviation occurs in prose but no full form
    co-occurs (zh accepts either the Chinese or the English full form)."""
    prose = list(prose_lines(text, drop_headings=True))
    page = "\n".join(line for _, line in prose).lower()
    hits = []
    for abbr, (zh_full, en_full) in sorted(terms.items()):
        pattern = _occurrences(abbr)
        found = [(n, line) for n, line in prose if pattern.search(line)]
        if not found:
            continue
        allowed = (
            [zh_full.lower(), en_full.lower()]
            if lang == "zh"
            else [en_full.lower()]
        )
        if not any(form in page for form in allowed):
            lineno, line = found[0]
            hits.append((lineno, f"{abbr}: {line.strip()}"))
    return hits


CANDIDATE_RE = re.compile(
    r"(?<![A-Za-z0-9])[A-Z][A-Z0-9]{1,}(?:-[A-Z0-9]+)*(?![A-Za-z0-9])"
)


def unknown_candidates_in_text(text: str, terms: dict | None = None):
    """Uppercase runs in prose that are neither controlled terms nor
    proper nouns; reported non-fatally so maintainers can curate them."""
    seen = set()
    known = set(terms if terms is not None else load_terms()) | PROPER_NOUNS
    for _lineno, line in prose_lines(text, drop_headings=True):
        for m in CANDIDATE_RE.finditer(line):
            token = m.group(0)
            if token.upper() not in known:
                seen.add(token)
    return sorted(seen)


def iter_manual_pages():
    for path in sorted(MANUAL_ROOT.rglob("*.md")):
        lang = path.parts[path.parts.index("user_manual") + 1]
        yield path, lang


def find_abbreviation_violations(terms: dict):
    out = []
    for path, lang in iter_manual_pages():
        text = path.read_text(encoding="utf-8")
        for lineno, msg in abbreviation_violations_in_text(text, terms, lang):
            out.append((str(path), lineno, msg))
    return out


# Rule A of the style guide: no ASCII space at a CJK/fullwidth <->
# Latin/digit boundary in prose. (Rule B -- boundaries against inline
# code spans, bracket links and emphasis -- is sweep-only, not guarded.)
_CJK = "一-鿿　-〿！-～"
SPACING_RE = re.compile(
    rf"(?<=[{_CJK}]) +(?=[A-Za-z0-9])|(?<=[A-Za-z0-9]) +(?=[{_CJK}])"
)


def spacing_violations_in_text(text: str):
    hits = []
    for lineno, line in prose_lines(text, drop_headings=False):
        m = SPACING_RE.search(line)
        if m:
            hits.append((lineno, line.strip()))
    return hits


def find_spacing_violations():
    out = []
    for path, _lang in iter_manual_pages():
        text = path.read_text(encoding="utf-8")
        for lineno, excerpt in spacing_violations_in_text(text):
            out.append((str(path), lineno, excerpt))
    return out
