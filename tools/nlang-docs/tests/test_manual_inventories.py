"""Pin the manual's stdlib inventory claims to the shipped tree: the
package counts named in the README and the manual (both language
editions) must agree with the contents of stdlib/; the library-chapter
range cited by the package-structure pages must span the actual
library chapter entries; and the binary-expression promotion page
must name every resolver family the source dispatches. Prose counts
rot the moment the next package or operator family lands, and these
inventories are a user-visible face of the manual-sync sweep, so the
checks live in the docs suite instead of in prose discipline."""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

# The preset packages are always in scope and are never counted among
# the importable standard packages the claims below name.
PRELUDE = {"lang", "containers"}

EN_WORDS = {"ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14}
ZH_WORDS = {"十": 10, "十一": 11, "十二": 12, "十三": 13, "十四": 14}

CLAIM_FILES = [
    "README.md",
    "README.zh-CN.md",
    "docs/user_manual/en/getting-started/install-layout.md",
    "docs/user_manual/zh/getting-started/install-layout.md",
    "docs/user_manual/en/specification/03-program-structure/packages.md",
    "docs/user_manual/zh/specification/03-program-structure/packages.md",
    "docs/user_manual/en/specification/library/11-conventions.md",
    "docs/user_manual/zh/specification/library/11-conventions.md",
]

# "the eleven standard packages", "the standard twelve packages", and
# the §11.1 wording "is thirteen packages" (no "standard").
EN_CLAIM = re.compile(
    r"\b(?:(ten|eleven|twelve|thirteen|fourteen) (?:standard )?packages"
    r"|standard (ten|eleven|twelve|thirteen|fourteen) packages)\b")
# "十二个标准包", "十二个包". 十 is required, so 每个包/一个包 never match.
ZH_CLAIM = re.compile(r"(十[一二三四]?)(?=个(?:标准)?包)")

PACKAGES_PAGES = [
    "docs/user_manual/en/specification/03-program-structure/packages.md",
    "docs/user_manual/zh/specification/03-program-structure/packages.md",
]
# Chapter ranges like "chapters 13–21" / "第13–21章" (en dash).
RANGE_CLAIM = re.compile(r"chapters (\d+)–(\d+)|第(\d+)–(\d+)章")
# The package chapters start at 13 (`math`); 11 (conventions) and 12
# (the base container classes) are library chapters but not packages.
FIRST_PACKAGE_CHAPTER = 13

PROMOTION_PAGES = [
    "docs/user_manual/en/vm-architecture/binary-expression-promotion.md",
    "docs/user_manual/zh/vm-architecture/binary-expression-promotion.md",
]


def _stdlib_standard_count() -> int:
    names = {p.stem for p in (REPO / "stdlib").glob("*.n")}
    missing = PRELUDE - names
    assert not missing, f"stdlib/ lost a preset package: {missing}"
    return len(names - PRELUDE)


def test_package_count_claims_match_stdlib():
    expected = _stdlib_standard_count()
    wrong = []
    claims = 0
    for rel in CLAIM_FILES:
        text = (REPO / rel).read_text(encoding="utf-8")
        for m in EN_CLAIM.finditer(text):
            claims += 1
            word = m.group(1) or m.group(2)
            if EN_WORDS[word] != expected:
                wrong.append(f"{rel}: '{word} packages' != {expected}")
        for m in ZH_CLAIM.finditer(text):
            claims += 1
            if ZH_WORDS[m.group(1)] != expected:
                wrong.append(f"{rel}: '{m.group(1)}个包' != {expected}")
    assert claims >= 2, "package-count claims vanished from the claim files"
    assert not wrong, "package counts disagree with stdlib/: " + "; ".join(wrong)


def test_library_chapter_range_claims_match_the_tree():
    lib_dir = REPO / "docs" / "user_manual" / "en" / "specification" / "library"
    # Chapters are numbered files and directories alike (14-io, 21-network).
    nums = sorted(int(p.name.split("-", 1)[0]) for p in lib_dir.iterdir()
                  if re.match(r"\d+-", p.name))
    last_chapter = nums[-1]
    checked = 0
    for rel in PACKAGES_PAGES:
        text = (REPO / rel).read_text(encoding="utf-8")
        for m in RANGE_CLAIM.finditer(text):
            checked += 1
            a, b = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
            assert (int(a), int(b)) == (FIRST_PACKAGE_CHAPTER, last_chapter), (
                f"{rel} cites chapters {a}–{b} but the library chapters "
                f"span {FIRST_PACKAGE_CHAPTER}–{last_chapter} for packages")
    assert checked == 2, "chapter-range claim vanished from the packages pages"


def test_promotion_page_names_every_resolver_family():
    # Access(SnBinaryExpr&) dispatches three families (comparison/logical
    # gates inline, ResolveShiftBinary, ResolveBitwise, and
    # ResolveArithmeticBinary as the fallback — ExprResolverBinary.cpp);
    # the mechanism page must name every one in both editions.
    for rel in PROMOTION_PAGES:
        text = (REPO / rel).read_text(encoding="utf-8")
        for name in ("ResolveShiftBinary", "ResolveBitwise",
                     "ResolveArithmeticBinary"):
            assert name in text, (
                f"{rel} does not name {name} - the resolver dispatch "
                f"description is stale against ExprResolverBinary.cpp")
