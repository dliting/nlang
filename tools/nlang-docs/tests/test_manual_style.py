import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from nlang_docs import manual_style as ms

GUIDE_SAMPLE = (
    "| 缩写 | 中文全称 | 英文全称 |\n"
    "|---|---|---|\n"
    "| PRNG | 伪随机数生成器 | pseudorandom number generator |\n"
)


def test_parse_term_table_reads_rows_not_separators():
    terms = ms.parse_term_table(GUIDE_SAMPLE)
    assert terms == {
        "PRNG": ("伪随机数生成器", "pseudorandom number generator")
    }


def test_abbreviation_needs_full_form_on_page():
    terms = ms.parse_term_table(GUIDE_SAMPLE)
    bad = "PRNG的种子决定序列。\n"
    assert ms.abbreviation_violations_in_text(bad, terms, "zh") != []
    good = "伪随机数生成器（PRNG，Pseudo Random Number Generator）的种子决定序列。\n"
    assert ms.abbreviation_violations_in_text(good, terms, "zh") == []
    good_en = "A pseudorandom number generator (PRNG) seeds the sequence.\n"
    assert ms.abbreviation_violations_in_text(good_en, terms, "en") == []


def test_abbreviation_headings_keep_short_form():
    terms = ms.parse_term_table(GUIDE_SAMPLE)
    text = "# PRNG概述\n\n正文不出现这个词。\n"
    assert ms.abbreviation_violations_in_text(text, terms, "zh") == []


def test_abbreviation_ignores_code_and_filenames():
    terms = ms.parse_term_table(GUIDE_SAMPLE)
    text = "种子写入 `PRNG_SEED`，程序 `prng_demo.n` 已编译。\n"
    assert ms.abbreviation_violations_in_text(text, terms, "zh") == []


def test_unknown_candidates_reports_unregistered_tokens():
    terms = ms.parse_term_table(GUIDE_SAMPLE)
    text = "这里提到 JNI 与 PRNG。\n"
    # PRNG is a controlled term; only JNI should surface as candidate.
    assert ms.unknown_candidates_in_text(text, terms) == ["JNI"]


def test_manual_tree_reaches_all_pages():
    assert sum(1 for _ in ms.iter_manual_pages()) > 80


def test_manual_tree_abbreviation_co_occurrence():
    terms = ms.load_terms()
    assert len(terms) >= 15
    assert ms.find_abbreviation_violations(terms) == []


def test_unknown_candidates_tree_report():
    # Informational only: print unregistered uppercase candidates per
    # page so maintainers can curate the guide's term table. No
    # assertion -- the candidate list legitimately changes with manual
    # content, pinning it would be brittle.
    terms = ms.load_terms()
    for path, lang in ms.iter_manual_pages():
        cands = ms.unknown_candidates_in_text(
            path.read_text(encoding="utf-8"), terms)
        if cands:
            print(f"[unknown-{lang}] {path.name}: {', '.join(cands)}")
