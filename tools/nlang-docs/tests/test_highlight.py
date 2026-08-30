"""NLangLexer token 断言：清单与 nlang.l 对齐，防漂移。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from pygments.token import Comment, Keyword, Number, String  # noqa: E402

from nlang_docs.highlight import NLANG_KEYWORDS, NLANG_TYPES, NLangLexer  # noqa: E402


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


def test_numbers_int_and_float():
    toks = dict(_tokens("1 2.5"))
    assert toks["1"] in Number
    assert toks["2.5"] in Number


def test_keyword_list_matches_scanner_surface():
    #锚定 nlang.l 全表（2026-08-30 提取）；清理 EN 残留关键字时
    #此清单必须同步——这就是防漂移机制。
    assert NLANG_KEYWORDS == frozenset(
        "as assert break case catch class const continue default do else "
        "elseif enum finally for foreach if implements import in interface "
        "namespace native new out return super switch this throw try using "
        "while".split())
    assert NLANG_TYPES == frozenset(
        "bool byte char float int short string ubyte uint ushort void "
        "Dict Func List".split())
