"""Lexical cross-check: the reserved-word table in §1.4 of the
specification equals the keyword set the lexer can produce. The word
face is always the rule's literal left side — the pattern admits both
action styles (a direct "return KT_X;" and a computed one, e.g. "class"
arming flags or every rule starting with the path-segment soft-read) —
because the KT_ token name is only the keyword filter: lower-casing its
suffix loses camelCase (KT_TypeName vs the word `typeName`)."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LEXER = REPO / "src" / "compiler" / "grammar" / "nlang.l"
SPEC_LEX = (REPO / "docs" / "user_manual" / "en" / "specification"
            / "01-lexical.md")

# "word" { ... return ... KT_Word; } -- the quoted left side is the word
# face; the KT_ token in the return statement filters keyword rules from
# the other quoted literals (the operators return OT_ tokens, the state
# rules return no value). Comments inside an action carry no braces, so
# the [^{}] spans stop at the rule boundary.
KEYWORD_RULE = re.compile(
    r'"([A-Za-z_#]+)"\s*\{[^{}]*\breturn\b[^;{}]*\bKT_\w+', re.S)
# spec table rows: | `word` | ... | (mixed case admitted — typeName is
# the language's first camelCase reserved word, v0.13.1)
TABLE_ROW = re.compile(r"^\|\s*`([A-Za-z_#]+)`\s*\|", re.M)


def _lexer_keywords():
    text = LEXER.read_text(encoding="utf-8", errors="replace")
    return set(KEYWORD_RULE.findall(text))


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
