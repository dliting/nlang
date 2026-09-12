"""NLang Pygments lexer for the docs site's ```nlang fences.

Token classes mirror the IDE SyntaxHighlighter semantics (whole-string
coloring for interpolated strings). The keyword/type sets are anchored
to the flex scanner's keyword table (src/compiler/grammar/nlang.l);
test_highlight extracts that table itself, so a scanner-side change
turns the suite red instead of drifting silently.

Number boundary: only plain decimal int/float literals, matching the
docs fences' usage — hex (0x1F), exponent (1.5e3), leading-dot (.5)
and signed forms (the scanner folds the sign into the literal) are
not distinguished.
"""
from pygments.lexer import RegexLexer
from pygments.token import (Comment, Keyword, Name, Number, Operator,
                            Punctuation, String, Whitespace)

#Pygments' registry loader (_load_lexers) imports this module and
#iterates __all__ taking .name on every entry, so __all__ must list
#the lexer class only; the keyword sets below stay module-level and
#are imported directly by the hook's consumers (never via import *).
__all__ = ["NLangLexer"]

#Full nlang.l keyword table (2026-08-30): 40 keywords + 11 builtin-type
#words (NLANG_TYPES below) + 3 constants (NLANG_CONSTANTS below) = the
#scanner's 54 reserved words. "state" has no parser production
#(%token only) but stays so lexer == scanner; test_highlight
#anchors all three sets to the scanner source directly.
NLANG_KEYWORDS = frozenset(
    "as assert break case catch class const continue default do else "
    "elseif enum finally for foreach if implements import in interface "
    "namespace native new out private protected public return state "
    "static struct super switch this throw try using virtual "
    "while".split())
NLANG_TYPES = frozenset(
    "bool byte char float int short string ubyte uint ushort void "
    "Dict Func List".split())
NLANG_CONSTANTS = frozenset("true false null".split())


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
            (r"\b(?:%s)\b" % "|".join(sorted(
                NLANG_CONSTANTS, key=len, reverse=True)), Keyword.Constant),
            (r"\b(?:%s)\b" % "|".join(sorted(
                NLANG_TYPES, key=len, reverse=True)), Keyword.Type),
            (r"\b\d+\.\d+\b", Number.Float),
            (r"\b\d+\b", Number.Integer),
            (r"\b[A-Za-z_][A-Za-z0-9_]*\b", Name),
            #Operators/punctuation sit after the word/number rules and
            #before the whitespace fallback: the comment rules above take
            #// and /* first, so neither rule can eat them. Without these
            #two every ; { } = + falls to Token.Error (uncolored, and it
            #disarms Error as the "unmatched" diagnostic).
            (r"[][(){}<>,;:.~?]", Punctuation),
            (r"[=+\-*/%&|^!]+", Operator),
            (r"\s+", Whitespace),
        ],
    }
