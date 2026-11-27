"""ABI version-stamp guard: the manual's native-module ABI chapter and
the host header agree on the current ABI version.

The 0.14.13 close-out caught a three-way drift the build never saw:
the header stamped 16, the manual chapter stamped 12, and the
string-length ABI (13 on master, 14-17 on the merged line) had landed
on both sides without either stamp following. The chapter is the
user-facing authority for the host table, so every version stamp in
it must equal NLANG_HOST_ABI_VERSION; this guard makes that
agreement mechanical instead of reviewable-by-eye.
"""
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
HEADER = REPO_ROOT / "include" / "nlang" / "vm" / "NativeHost.h"

#The version appears in several independent places in each chapter:
#the section-30.1 prose, the load-sequence step, the exports table
#and the table-layout paragraph. Each language edition carries its
#own phrasing, so the per-language patterns list every stamp the
#guard reads; a reword that drops a stamp is caught by the minimum
#count, and a reword that changes one stamp is caught by the
#equality check.
_STAMPS_EN = (
    r"describes ABI version \*\*(\d+)\*\*",
    r"reports exactly `(\d+)`",
    r"other than `(\d+)` is rejected",
    r"returns `(\d+)`; the single version authority",
    r"holding `(\d+)`,",
)
_STAMPS_ZH = (
    r"描述ABI版本 \*\*(\d+)\*\*",
    r"恰好报`(\d+)`的模块",
    r"除`(\d+)`以外的值被拒绝",
    r"返回`(\d+)`；唯一的版本权威",
    r"持有`(\d+)`的`uint32_t`",
)

_DEFINE_RE = re.compile(r"#define NLANG_HOST_ABI_VERSION (\d+)u")


def _header_abi():
    m = _DEFINE_RE.search(HEADER.read_text(encoding="utf-8"))
    assert m, "NLANG_HOST_ABI_VERSION not found in NativeHost.h"
    return int(m.group(1))


def test_header_defines_a_version():
    #A silently broken header parse would make every stamp check pass
    #against 0; pin the shape so the parse failure is visible here.
    assert _header_abi() >= 1


def test_en_manual_stamps_match_header():
    expected = _header_abi()
    text = (REPO_ROOT
            / "docs/user_manual/en/specification/formats/30-abi.md"
            ).read_text(encoding="utf-8")
    stamps = [int(m.group(1))
              for pattern in _STAMPS_EN
              for m in [re.search(pattern, text)] if m]
    assert len(stamps) == len(_STAMPS_EN), (
        "not every ABI version stamp was found in the en manual — "
        "did the chapter reword one? patterns: "
        + ", ".join(_STAMPS_EN))
    assert set(stamps) == {expected}, (
        f"en manual ABI stamps {sorted(set(stamps))} disagree with "
        f"NLANG_HOST_ABI_VERSION {expected}")


def test_zh_manual_stamps_match_header():
    expected = _header_abi()
    text = (REPO_ROOT
            / "docs/user_manual/zh/specification/formats/30-abi.md"
            ).read_text(encoding="utf-8")
    stamps = [int(m.group(1))
              for pattern in _STAMPS_ZH
              for m in [re.search(pattern, text)] if m]
    assert len(stamps) == len(_STAMPS_ZH), (
        "not every ABI version stamp was found in the zh manual — "
        "did the chapter reword one? patterns: "
        + ", ".join(_STAMPS_ZH))
    assert set(stamps) == {expected}, (
        f"zh manual ABI stamps {sorted(set(stamps))} disagree with "
        f"NLANG_HOST_ABI_VERSION {expected}")
