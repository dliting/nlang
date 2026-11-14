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

#The scanner's word tables (0.13.1c): keywords (NLANG_KEYWORDS),
#builtin-type words (NLANG_TYPES: the scanner's type keywords plus the
#docs-side generic names Dict/List), and the literal constants
#(NLANG_CONSTANTS). Every keyword and type word is anchored to the
#scanner source directly by test_highlight, so this table cannot
#silently drift from nlang.l — counts are intentionally not stated
#here; they rot (as this comment's own history shows).
#(Removals along the way: "namespace" (Phase 5), "new" (0.9.0), and
#"elseif"/"state" (0.13.1c) are ordinary identifiers now; the 0.7.5
#double family added "double"/"long" to the type words; 0.8.3 made
#"func" a scanner keyword, so the docs side moved it from NLANG_TYPES
#to NLANG_KEYWORDS; 0.13.1c added the width-explicit primitive
#aliases (int8..int64, uint8..uint64, fp32/fp64) to the type words.)
NLANG_KEYWORDS = frozenset(
    "as assert break case catch class const continue default do else "
    "enum finally for foreach func if implements import in "
    "interface native out private protected public return "
    "static struct super switch this throw try typeName using virtual "
    "while".split())
NLANG_TYPES = frozenset(
    "bool byte char double float int long short string ubyte uint "
    "ulong ushort void fp32 fp64 int8 int16 int32 int64 uint8 uint16 "
    "uint32 uint64 Dict List".split())
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
