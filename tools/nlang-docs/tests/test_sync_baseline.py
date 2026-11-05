"""SYNC_BASELINE sanity - the manual's coverage anchor must be a
readable file whose first non-comment line looks like a commit hash
(the drift report itself is tools/manual_sync_check.py, run as a
release gate; this test only pins the anchor's shape so a renamed or
emptied file fails the docs suite instead of silently disabling the
gate)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
BASELINE = REPO_ROOT / "docs" / "user_manual" / "SYNC_BASELINE"


def test_baseline_file_exists_and_carries_a_hash():
    assert BASELINE.is_file(), (
        "docs/user_manual/SYNC_BASELINE is missing - the manual's "
        "sync anchor (see AGENTS.md) must exist"
    )
    hashes = [
        line.strip().split()[0]
        for line in BASELINE.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    assert hashes, (
        "SYNC_BASELINE carries no hash on its first non-comment line"
    )
    #A short (12 hex) or full (40 hex) git object id.
    assert all(len(h) in (7, 8, 12, 40) for h in hashes[:1]), (
        f"baseline hash {hashes[0]!r} does not look like a git commit id"
    )
