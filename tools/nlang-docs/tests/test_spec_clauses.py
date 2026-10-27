"""Clause numbering: chapter H1 matches the file-name prefix, section
numbers are dense within a chapter and unique per page, zh mirrors the
en numbering, and every section reference (§n.k[.m]) resolves."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from test_spec_structure import SPEC_EN, SPEC_ZH  # noqa: E402

H1 = re.compile(r"^# (\d+)\. ", re.M)
NUM_HEAD = re.compile(r"^(#{2,4}) (\d+(?:\.\d+)*) ", re.M)
REF = re.compile(r"§(\d+(?:\.\d+)*)")


def _pages():
    for tree in (SPEC_EN, SPEC_ZH):
        if tree and tree.exists():
            yield from tree.rglob("*.md")


def _numbers(page):
    text = page.read_text(encoding="utf-8")
    nums = [m.group(2) for m in NUM_HEAD.finditer(text)]
    h1 = H1.search(text)
    if h1:
        nums.insert(0, h1.group(1))
    return text, nums


def test_h1_matches_file_prefix():
    for page in _pages():
        text, _ = _numbers(page)
        h1 = H1.search(text)
        if page.name == "index.md":
            assert h1 is None, f"{page}: index carries no clause number"
            continue
        assert h1, f"{page}: missing '# <n>. ' heading"
        assert h1.group(1).zfill(2) == page.name[:2], \
            f"{page}: H1 number {h1.group(1)} != file prefix"


def test_numbers_unique_and_dense():
    for page in _pages():
        _, nums = _numbers(page)
        assert len(nums) == len(set(nums)), f"{page}: duplicate clause number"
        kids = {}
        for n in nums:
            if "." in n:
                parent, _, leaf = n.rpartition(".")
                kids.setdefault(parent, []).append(int(leaf))
        for parent, leaves in kids.items():
            assert sorted(leaves) == list(range(1, len(leaves) + 1)), \
                f"{page}: gaps under {parent}: {sorted(leaves)}"


def test_zh_numbers_mirror_en():
    # a zh page never invents its own numbering: its clause-number set
    # must equal the same-named en page's set
    if not (SPEC_ZH and SPEC_ZH.exists()):
        return
    for zh_page in SPEC_ZH.rglob("*.md"):
        en_page = SPEC_EN / zh_page.relative_to(SPEC_ZH)
        if not en_page.exists():
            continue  # counterpart absence is tree-parity's business
        _, zh_nums = _numbers(zh_page)
        _, en_nums = _numbers(en_page)
        assert set(zh_nums) == set(en_nums), \
            f"{zh_page}: numbering diverges from {en_page.name}"


def test_refs_resolve():
    defined = set()
    for page in _pages():
        _, nums = _numbers(page)
        defined.update(nums)
    for page in _pages():
        text, _ = _numbers(page)
        for m in REF.finditer(text):
            assert m.group(1) in defined, \
                f"{page}: §{m.group(1)} has no definition"
