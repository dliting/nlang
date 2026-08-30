"""NLang Pygments lexer for the docs site's ```nlang fences.

Token classes mirror the IDE SyntaxHighlighter semantics (whole-string
coloring for interpolated strings). The keyword/type sets are anchored
to the flex scanner's keyword table (src/compiler/grammar/nlang.l);
test_highlight pins them so a scanner cleanup forces a sync here.
"""
from pygments.lexer import RegexLexer
from pygments.token import Comment, Keyword, Name, Number, String, Whitespace

__all__ = ["NLangLexer", "NLANG_KEYWORDS", "NLANG_TYPES"]

#Full nlang.l keyword table (2026-08-30): 40 keywords + 11 builtin-type
#words (in NLANG_TYPES below) + 3 constants (true/false/null, their own
#rule below) = the scanner's 54 reserved words. "state" has no parser
#production (EN residue, %token only) but stays so lexer == scanner;
#when the scanner's table changes, test_highlight forces a sync here.
NLANG_KEYWORDS = frozenset(
    "as assert break case catch class const continue default do else "
    "elseif enum finally for foreach if implements import in interface "
    "namespace native new out private protected public return state "
    "static struct super switch this throw try using virtual "
    "while".split())
NLANG_TYPES = frozenset(
    "bool byte char float int short string ubyte uint ushort void "
    "Dict Func List".split())


class NLangLexer(RegexLexer):
    """Serves ```nlang fences via the LEXERS registry injection done
    by highlight_hook (mkdocs hooks: entry)."""

    name = "NLang"
    aliases = ["nlang"]
    filenames = ["*.n"]
    mimetypes = ["text/x-nlang"]

    tokens = {
        "root": [
            (r"//.*\n", Comment.Single),
            #(?s:) scopes DOTALL onto the lazy body only — pygments rule
            #tuples carry no per-rule flags slot (a 3rd element is parsed
            #as a state), and a class-level DOTALL would let //.*\n
            #swallow every following line.
            (r"/\*(?s:.*?)\*/", Comment.Multiline),
            (r'"(?:[^"\\]|\\.)*"', String),
            (r"\b(?:%s)\b" % "|".join(sorted(
                NLANG_KEYWORDS, key=len, reverse=True)), Keyword),
            (r"\b(?:true|false|null)\b", Keyword.Constant),
            (r"\b(?:%s)\b" % "|".join(sorted(
                NLANG_TYPES, key=len, reverse=True)), Keyword.Type),
            (r"\b\d+\.\d+\b", Number.Float),
            (r"\b\d+\b", Number.Integer),
            (r"\b[A-Za-z_][A-Za-z0-9_]*\b", Name),
            (r"\s+", Whitespace),
        ],
    }
