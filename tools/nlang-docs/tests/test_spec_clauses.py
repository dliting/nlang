"""Clause numbering: a chapter is a `NN-` prefixed file or directory.
A chapter's clause numbers are unique and dense across every page it
contains, each page's H1 matches the chapter number derived from its
path (chapter pages `# n. `, chapter-directory subpages `# n.k `), zh
mirrors the en numbering page by page, `reference/` pages are
informative and exempt from numbering, and every section reference
(§n.k[.m]) resolves against the numbered headings of the normative
pages of the whole tree."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from test_spec_structure import SPEC_EN, SPEC_ZH  # noqa: E402

# "# 4. Types" (chapter page) and "# 4.2 Primitives" (directory subpage)
H1 = re.compile(r"^# (\d+(?:\.\d+)*)\.? ", re.M)
NUM_HEAD = re.compile(r"^(#{2,4}) (\d+(?:\.\d+)*) ", re.M)
REF = re.compile(r"§(\d+(?:\.\d+)*)")
CHAPTER = re.compile(r"^(\d{2})-")


def _pages(tree):
    if tree and tree.exists():
        yield from tree.rglob("*.md")


def _numbers(page):
    text = page.read_text(encoding="utf-8")
    nums = [m.group(2) for m in NUM_HEAD.finditer(text)]
    h1 = H1.search(text)
    if h1:
        nums.insert(0, h1.group(1))
    return text, nums


def _chapter(page, tree):
    """The chapter a page belongs to, derived from its path: a
    (kind, number) pair, or None for informative `reference/` pages.

    kind is "file" for a `NN-*.md` chapter file (at the tree root or
    inside a grouping container such as `library/`), "index" for the
    index.md of a `NN-*/` chapter directory, "sub" for that
    directory's subpages, and "plain" for an unnumbered index.md
    outside any chapter. A chapter directory sits at the tree root or
    directly inside a grouping container, and its pages sit exactly
    one level below it: the tree nests at most chapter directory ->
    page.
    """
    rel = page.relative_to(tree)
    parts = rel.parts
    if parts[0] == "reference":
        return None
    # a chapter directory is the first `NN-` path component, at the
    # tree root (depth 0) or inside a grouping container (depth 1)
    for depth in (0, 1):
        if len(parts) <= depth + 1:
            break
        m = CHAPTER.match(parts[depth])
        if m:
            if len(parts) != depth + 2:
                raise AssertionError(
                    f"{page}: pages sit exactly one level below a "
                    f"chapter directory")
            kind = "index" if rel.name == "index.md" else "sub"
            return (kind, int(m.group(1)))
    m = CHAPTER.match(parts[-1])
    if m:
        return ("file", int(m.group(1)))
    if rel.name == "index.md":
        return ("plain", None)
    raise AssertionError(f"{page}: not inside a chapter")


def _check_h1(tree):
    for page in _pages(tree):
        text, _ = _numbers(page)
        h1 = H1.search(text)
        chapter = _chapter(page, tree)
        if chapter is None:
            continue  # informative reference page
        kind, number = chapter
        if kind == "plain":
            assert h1 is None, f"{page}: index carries no clause number"
        elif kind == "sub":
            assert h1 and h1.group(1).count(".") == 1 and \
                int(h1.group(1).partition(".")[0]) == number, \
                f"{page}: subpage H1 must be '# {number}.k <title>'"
        else:
            assert h1 and "." not in h1.group(1) and \
                int(h1.group(1)) == number, \
                f"{page}: H1 number {h1.group(1) if h1 else None!r} " \
                f"does not match chapter {number:02d} from the path"


def _check_numbers(tree):
    chapters = {}
    for page in _pages(tree):
        chapter = _chapter(page, tree)
        if chapter is None or chapter[0] == "plain":
            continue
        rel = page.relative_to(tree)
        # one bucket per chapter: the file itself, or its directory
        key = (rel.as_posix() if chapter[0] == "file"
               else rel.parent.as_posix())
        chapters.setdefault(key, []).append(page)
    for key, pages in chapters.items():
        nums = []
        for page in pages:
            _, page_nums = _numbers(page)
            nums.extend(page_nums)
        assert len(nums) == len(set(nums)), \
            f"{tree / key}: duplicate clause number"
        kids = {}
        for n in nums:
            if "." in n:
                parent, _, leaf = n.rpartition(".")
                kids.setdefault(parent, []).append(int(leaf))
        for parent, leaves in kids.items():
            assert sorted(leaves) == list(range(1, len(leaves) + 1)), \
                f"{tree / key}: gaps under {parent}: {sorted(leaves)}"


def _check_zh_mirrors(en_tree, zh_tree):
    # a zh page never invents its own numbering: its clause-number set
    # must equal the same-named en page's set
    if not (zh_tree and zh_tree.exists()):
        return
    for zh_page in zh_tree.rglob("*.md"):
        if _chapter(zh_page, zh_tree) is None:
            continue  # informative pages carry no clause numbering
        en_page = en_tree / zh_page.relative_to(zh_tree)
        if not en_page.exists():
            continue  # counterpart absence is tree-parity's business
        _, zh_nums = _numbers(zh_page)
        _, en_nums = _numbers(en_page)
        assert set(zh_nums) == set(en_nums), \
            f"{zh_page}: numbering diverges from {en_page.name}"


def _check_refs(trees):
    # the resolution domain is the numbered headings of the normative
    # pages; informative reference pages define no clauses
    defined = set()
    for tree in trees:
        for page in _pages(tree):
            if _chapter(page, tree) is None:
                continue
            _, nums = _numbers(page)
            defined.update(nums)
    # references written on any page, informative ones included,
    # must still resolve
    for tree in trees:
        for page in _pages(tree):
            text, _ = _numbers(page)
            for m in REF.finditer(text):
                assert m.group(1) in defined, \
                    f"{page}: §{m.group(1)} has no definition"


def test_h1_matches_path_number():
    for tree in (SPEC_EN, SPEC_ZH):
        _check_h1(tree)


def test_numbers_unique_and_dense():
    for tree in (SPEC_EN, SPEC_ZH):
        _check_numbers(tree)


def test_zh_numbers_mirror_en():
    _check_zh_mirrors(SPEC_EN, SPEC_ZH)


def test_refs_resolve():
    _check_refs((SPEC_EN, SPEC_ZH))


def _write(root, rel, text):
    page = root / rel
    page.parent.mkdir(parents=True, exist_ok=True)
    page.write_text(text, encoding="utf-8")


def test_chapter_directory_passes(tmp_path):
    # a `NN-` directory is one chapter: index.md carries `# n. `, a
    # subpage carries `# n.k ` continuing where the previous page ended
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n\n## 4.1 Overview\n")
    _write(en, "04-types/primitives.md",
           "# 4.2 Primitives\n\n## 4.2.1 Integers\n\nsee §4.1\n")
    _check_h1(en)
    _check_numbers(en)
    _check_refs((en,))


def test_chapter_directory_gap_is_red(tmp_path):
    # 4.1 appears nowhere in the chapter, so 4.2 leaves a gap under 4
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n")
    _write(en, "04-types/primitives.md", "# 4.2 Primitives\n")
    with pytest.raises(AssertionError, match=r"gaps under 4"):
        _check_numbers(en)


def test_chapter_directory_duplicate_across_pages_is_red(tmp_path):
    # clause numbers are unique across every page of the chapter
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n\n## 4.1 Overview\n")
    _write(en, "04-types/a.md", "# 4.2 A\n")
    _write(en, "04-types/b.md", "# 4.2 B\n")
    with pytest.raises(AssertionError, match="duplicate clause number"):
        _check_numbers(en)


def test_reference_page_is_exempt(tmp_path):
    # informative reference pages carry no clause numbering at all
    en = tmp_path / "en"
    _write(en, "reference/foo.md", "# Common Errors\n\nplain prose\n")
    _check_h1(en)
    _check_numbers(en)
    _check_refs((en,))


def test_container_chapter_file_follows_prefix_rule(tmp_path):
    # `NN-` files inside grouping containers keep the per-file rule
    en = tmp_path / "en"
    _write(en, "library/12-collections.md",
           "# 12. Collections\n\n## 12.1 List\n")
    _check_h1(en)
    _check_numbers(en)
    _write(en, "library/13-math.md", "# 14. Math\n")
    with pytest.raises(AssertionError, match="H1"):
        _check_h1(en)


def test_container_chapter_directory_passes(tmp_path):
    # chapter directories also live inside grouping containers
    en = tmp_path / "en"
    _write(en, "library/14-io/index.md", "# 14. I/O\n\n## 14.1 Streams\n")
    _write(en, "library/14-io/streams.md", "# 14.2 Files\n")
    _check_h1(en)
    _check_numbers(en)


def test_container_chapter_directories_bucket_separately(tmp_path):
    # each chapter directory is policed on its own: the gap in
    # library/14-io is reported against that directory even though
    # the sibling library/16-x is dense
    en = tmp_path / "en"
    _write(en, "library/14-io/index.md", "# 14. I/O\n")
    _write(en, "library/14-io/streams.md", "# 14.2 Streams\n")
    _write(en, "library/16-x/index.md", "# 16. X\n\n## 16.1 A\n")
    _write(en, "library/16-x/b.md", "# 16.2 B\n")
    with pytest.raises(AssertionError, match=r"14-io: gaps under 14"):
        _check_numbers(en)


def test_deeper_nesting_is_red(tmp_path):
    # the tree nests at most chapter directory -> page
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n\n## 4.1 Overview\n")
    _write(en, "04-types/sub/x.md", "# 4.2 X\n")
    with pytest.raises(AssertionError, match="one level below"):
        _check_h1(en)


def test_subpage_wrong_chapter_is_red(tmp_path):
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n\n## 4.1 Overview\n")
    _write(en, "04-types/primitives.md", "# 5.2 Primitives\n")
    with pytest.raises(AssertionError, match="subpage H1"):
        _check_h1(en)


def test_chapter_directory_index_without_number_is_red(tmp_path):
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# Types\n")
    with pytest.raises(AssertionError, match="H1"):
        _check_h1(en)


def test_zh_mirror_mismatch_in_chapter_directory_is_red(tmp_path):
    en = tmp_path / "en"
    zh = tmp_path / "zh"
    _write(en, "04-types/index.md", "# 4. Types\n\n## 4.1 Overview\n")
    _write(en, "04-types/primitives.md", "# 4.2 Primitives\n")
    _write(zh, "04-types/index.md", "# 4. 类型\n\n## 4.1 概述\n")
    _write(zh, "04-types/primitives.md", "# 4.2 原始类型\n\n## 4.2.1 整数\n")
    with pytest.raises(AssertionError, match="diverges"):
        _check_zh_mirrors(en, zh)


def test_reference_heading_does_not_define_a_clause(tmp_path):
    # a stray numbered heading on an informative page must not make a
    # dangling §ref resolve
    en = tmp_path / "en"
    _write(en, "04-types/index.md", "# 4. Types\n\nsee §4.9\n")
    _write(en, "reference/foo.md", "# Common Errors\n\n## 4.9 Errors\n")
    with pytest.raises(AssertionError, match="§4.9 has no definition"):
        _check_refs((en,))
