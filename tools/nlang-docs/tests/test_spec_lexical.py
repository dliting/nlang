"""Lexical cross-check: the reserved-word table in §1.4 of the
specification equals the keyword set the lexer can produce. The set
is the union of the literal rules ("word" { return KT_Word; }) and
the keyword rules whose action computes before returning (e.g.
"func" sets the generic-open flag first); the KT_ token name maps
back to the word by lower-casing the suffix."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LEXER = REPO / "src" / "compiler" / "grammar" / "nlang.l"
SPEC_LEX = (REPO / "docs" / "user_manual" / "en" / "specification"
            / "01-lexical.md")

# "word" { return KT_Word; }  -- the direct literals
LITERAL_KW = re.compile(r'"([A-Za-z_#]+)"\s*\{\s*return\s+(KT_\w+)')
ALL_KT = re.compile(r"return\s+(KT_\w+)")
# spec table rows: | `word` | ... |
TABLE_ROW = re.compile(r"^\|\s*`([a-z_#]+)`\s*\|", re.M)


def _lexer_keywords():
    text = LEXER.read_text(encoding="utf-8", errors="replace")
    literals = {w for w, _ in LITERAL_KW.findall(text)}
    literal_tokens = {t for _, t in LITERAL_KW.findall(text)}
    computed = {t[3:].lower()
                for t in set(ALL_KT.findall(text)) - literal_tokens}
    return literals | computed


def _spec_reserved_table():
    text = SPEC_LEX.read_text(encoding="utf-8")
    reserved_sec = text.split("## 1.4 Reserved Words", 1)[1].split("\n## ", 1)[0]
    return set(TABLE_ROW.findall(reserved_sec))


def test_reserved_words_match_lexer():
    lexer_words = _lexer_keywords()
    spec_words = _spec_reserved_table()
    assert spec_words == lexer_words, (
        f"spec missing {sorted(lexer_words - spec_words)}, "
        f"spec extra {sorted(spec_words - lexer_words)}")
