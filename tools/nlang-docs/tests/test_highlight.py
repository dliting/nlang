"""NLangLexer token assertions, anchored to the flex scanner (nlang.l)."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from pygments.token import Comment, Error, Keyword, Number, String  # noqa: E402

from nlang_docs.highlight import (  # noqa: E402
    NLANG_CONSTANTS, NLANG_KEYWORDS, NLANG_TYPES, NLangLexer)

_SCANNER = (Path(__file__).resolve().parents[3]
            / "src" / "compiler" / "grammar" / "nlang.l")


def _tokens(code):
    #value→type pairs: dict(_tokens(code)) answers toks["import"] is Keyword
    return [(tokv, tokt) for tokt, tokv in NLangLexer().get_tokens(code)]


def test_keywords_and_types_color_distinctly():
    toks = dict(_tokens("import class int foreach List"))
    assert toks["import"] is Keyword
    assert toks["class"] is Keyword
    assert toks["int"] is Keyword.Type
    assert toks["foreach"] is Keyword
    assert toks["List"] is Keyword.Type


def test_constants_true_false_null():
    toks = dict(_tokens("true false null"))
    assert all(toks[w] is Keyword.Constant for w in ("true", "false", "null"))


def test_string_with_escape_and_interpolation_stays_whole():
    vals = [t for t, v in _tokens('string s = "a\\n${x}b";')
            if v in String]
    assert '"a\\n${x}b"' == "".join(vals)


def test_comments_both_kinds():
    toks = _tokens("int a; // line\nint b; /*--- block ---*/")
    assert any(v in Comment and "line" in t for t, v in toks)
    assert any(v in Comment and "block" in t for t, v in toks)
    #Regression face of the (?s:) multiline rule: a block comment spanning
    #lines must stay one token (a plain .*? would stop at the first \n).
    spans = [t for t, v in _tokens("/*--- a\nb\nc ---*/") if v in Comment]
    assert ["/*--- a\nb\nc ---*/"] == spans


def test_numbers_int_and_float():
    toks = dict(_tokens("1 2.5"))
    assert toks["1"] in Number
    assert toks["2.5"] in Number


def test_realistic_program_has_no_error_tokens():
    #Unmatched characters fall to Token.Error, so a whole real program
    #(imports, calls, arithmetic, strings, comments) must yield none —
    #this is the guard that catches a missing punctuation/operator rule.
    program = (
        "import io;\n"
        "\n"
        "int add(int a, int b) {\n"
        "    return a + b * 2;\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    io.print(\"sum: \" + add(1, 2));  // greet\n"
        "}\n")
    errors = [t for t, v in _tokens(program) if v is Error]
    assert [] == errors


def test_keyword_list_matches_scanner_surface():
    #Drift guard: the scanner's reserved words are extracted from the
    #flex source itself, so a keyword added or removed in nlang.l turns
    #this red instead of silently diverging from the lexer's sets.
    scanner = frozenset(re.findall(
        r'"([A-Za-z]+)"\s*\{\s*return KT_\w+;',
        _SCANNER.read_text(encoding="utf-8")))
    #List/Dict/Func are docs-side builtin types, not scanner words; the
    #exact pin turns red both when a stray word joins NLANG_TYPES and
    #when the scanner adopts one of the three.
    docs_types = NLANG_TYPES - scanner
    assert frozenset({"List", "Dict", "Func"}) == docs_types
    assert scanner == ((NLANG_KEYWORDS | NLANG_TYPES | NLANG_CONSTANTS)
                       - docs_types)
