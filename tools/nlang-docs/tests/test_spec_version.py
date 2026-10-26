"""The specification's version anchor: both index pages state the
version the repository currently builds. The anchor is prose, so the
only protection against drift is this check against the VERSION file
(the same file the site-footer injection reads)."""
from pathlib import Path

from test_spec_structure import SPEC_EN, SPEC_ZH

REPO = Path(__file__).resolve().parents[3]


def test_version_anchor_matches_version_file():
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    checked = 0
    for tree in (SPEC_EN, SPEC_ZH):
        index = tree / "index.md"
        if not index.exists():
            continue
        checked += 1
        text = index.read_text(encoding="utf-8")
        assert f"NLang {version}" in text, \
            f"{index}: version anchor drifted from VERSION ({version})"
    assert checked == 2, "both index pages must exist to pin the anchor"
