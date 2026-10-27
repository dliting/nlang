"""Specification tree structure: every page in the normative tree is
listed in its language nav, and no page leaks internal progress
references (docs/dev paths, phase numbers, commit hashes) that the
public-text and manual-style guards do not catch."""
import re
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.linkcheck import _ConfigLoader  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
SPEC_EN = REPO / "docs" / "user_manual" / "en" / "specification"
SPEC_ZH = REPO / "docs" / "user_manual" / "zh" / "specification"

# NLEP references carry the number only (NLEP-0001), never a path.
INTERNAL_REFS = re.compile(
    r"docs/dev/|docs/superpowers/|docs/user-manual-style-guide|temp/"
    r"|\bPhase\s+\d|\b[0-9a-f]{8}\b(?<!\d{8})")


def _nav_paths(config_name: str) -> list:
    doc = yaml.load((REPO / "docs" / config_name).read_text(encoding="utf-8"),
                    Loader=_ConfigLoader)
    flat = []

    def walk(items):
        for item in items:
            if isinstance(item, str):
                flat.append(item)
            elif isinstance(item, dict):
                for value in item.values():
                    walk(value if isinstance(value, list) else [value])

    walk(doc["nav"])
    return flat


def _md_rel(tree: Path) -> set:
    return {p.relative_to(tree.parent).as_posix()
            for p in tree.rglob("*.md")}


def test_en_spec_pages_all_in_nav():
    if not SPEC_EN.exists():
        return  # tree not seeded yet (guards run before the first page)
    assert _md_rel(SPEC_EN) <= set(_nav_paths("mkdocs.en.yml"))


def test_zh_spec_pages_all_in_nav():
    if not SPEC_ZH.exists():
        return
    assert _md_rel(SPEC_ZH) <= set(_nav_paths("mkdocs.zh.yml"))


def test_no_internal_refs():
    for tree in (SPEC_EN, SPEC_ZH):
        if not tree.exists():
            continue
        for page in tree.rglob("*.md"):
            text = page.read_text(encoding="utf-8")
            m = INTERNAL_REFS.search(text)
            assert m is None, f"{page}: internal ref {m.group(0)!r}"
