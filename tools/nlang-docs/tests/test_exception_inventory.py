"""Exception-inventory guard: every exception class the standard library
declares is named by the §8.3 exception-hierarchy page, in both language
editions.

The final review of the url/system packages caught the manual still
saying "three package-declared classes" after url and system each
declared a new one. This guard ties the §8.3 prose to the stdlib source,
so the next package exception cannot ship without its manual mention.
"""
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
STDLIB_DIR = REPO_ROOT / "stdlib"
MANUAL_PAGES = (
    REPO_ROOT / "docs/user_manual/en/specification/08-exceptions/index.md",
    REPO_ROOT / "docs/user_manual/zh/specification/08-exceptions/index.md",
)

_CLASS_RE = re.compile(r"^const class (\w+Exception)\b", re.MULTILINE)


def _declared_exception_classes():
    names = set()
    for path in sorted(STDLIB_DIR.glob("*.n")):
        names.update(_CLASS_RE.findall(path.read_text(encoding="utf-8")))
    return names


def test_every_declared_exception_is_documented():
    names = _declared_exception_classes()
    #A silently empty scan would make the guard vacuously green.
    assert names, "no exception classes found in stdlib — did the scan break?"
    for manual in MANUAL_PAGES:
        text = manual.read_text(encoding="utf-8")
        missing = [n for n in sorted(names) if n not in text]
        assert not missing, f"{manual.relative_to(REPO_ROOT)}: {missing}"
