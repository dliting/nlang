# mkdocs hook: register the NLang lexer before rendering. Loaded via
# the 'hooks:' list in mkdocs.yml (resolved relative to the config
# file; an absolute path stays absolute). Self-sufficient: appends
# this package's src/ to sys.path so a bare 'mkdocs build --strict'
# works without PYTHONPATH.
import sys
from pathlib import Path

_SRC = Path(__file__).resolve().parents[1]
if str(_SRC) not in sys.path:
    sys.path.append(str(_SRC))

from nlang_docs.highlight import NLangLexer  # noqa: E402

from pygments.lexers import LEXERS  # noqa: E402

#Registry-injection tuple: (module, lexer-name-attr, names, globs, mimes)
#— element 2 is the lexer's 'name' VALUE ('NLang'), not the class name.
#_load_lexers imports the module and iterates its __all__ taking .name
#on every entry, so highlight.py keeps __all__ to the lexer class alone
#(its keyword constants must not appear there).
LEXERS.setdefault(
    "NLangLexer",
    ("nlang_docs.highlight", "NLang", ("nlang",), ("*.n",),
     ("text/x-nlang",)))


def on_pre_build(config):
    #Import-time registration above already ran; this hook only needs
    #to exist so mkdocs imports the file before rendering pages.
    return None
