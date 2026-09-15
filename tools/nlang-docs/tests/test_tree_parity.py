"""Bilingual tree parity: every page either has a counterpart in the
other language tree or is listed in docs/translation-pending.txt. An
empty pending list turns the gate strict (any missing counterpart
fails), so the trees can be seeded first and translated in batches
without silent gaps. The two navs must list identical page paths once
the pending list is empty -- a page missing from one nav stays
invisible even when translated."""
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[3]
ZH_TREE = REPO / "docs" / "zh"
EN_TREE = REPO / "docs" / "en"
PENDING_FILE = REPO / "docs" / "translation-pending.txt"


class _PermissiveLoader(yaml.SafeLoader):
    """The configs carry ``!!python/name:`` values (toc slugify); treat
    them as opaque strings (same tolerance as the linkcheck loader)."""

    pass


_PermissiveLoader.add_constructor(
    "!python/name", lambda loader, node: loader.construct_scalar(node))


def _md_rel(tree: Path) -> set:
    return {p.relative_to(tree).as_posix() for p in tree.rglob("*.md")}


def _pending() -> set:
    if not PENDING_FILE.exists():
        return set()
    return {line.strip() for line
            in PENDING_FILE.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")}


def _nav_paths(config_name: str) -> list:
    doc = yaml.load((REPO / config_name).read_text(encoding="utf-8"),
                    Loader=_PermissiveLoader)
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


def test_tree_parity_with_pending_list():
    zh, en, pending = _md_rel(ZH_TREE), _md_rel(EN_TREE), _pending()
    #Pending entries carry the tree prefix; the same prefix turns the
    #relative page sets into comparable keys.
    missing = {f"zh/{page}" for page in zh - en} \
        | {f"en/{page}" for page in en - zh}
    unlisted = missing - pending
    stale = pending - missing
    assert not unlisted, f"pages without counterpart, not listed: {sorted(unlisted)}"
    assert not stale, f"pending entries already translated: {sorted(stale)}"


def test_pending_entries_name_real_pages():
    zh, en = _md_rel(ZH_TREE), _md_rel(EN_TREE)
    for entry in _pending():
        tree, _, rel = entry.partition("/")
        assert tree in ("zh", "en"), entry
        #The entry names the page that EXISTS and lacks its opposite.
        assert rel in (zh if tree == "zh" else en), entry


def test_nav_paths_identical_when_pending_empty():
    if _pending():
        pytest.skip("strict nav parity locks when translation completes")
    assert _nav_paths("mkdocs.zh.yml") == _nav_paths("mkdocs.en.yml")
