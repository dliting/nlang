# mkdocs hook: register the NLang lexer before rendering. Loaded via
# the 'hooks:' list in mkdocs.yml (resolved relative to the config
# file; an absolute path stays absolute).
#
#Self-sufficiency without PYTHONPATH: mkdocs execs this file at config
#validation and restores sys.path afterwards (Hooks._load_hook's
#finally), so the append below only lives long enough for the eager
#import to succeed — and that import is the load-bearing line: it
#leaves nlang_docs.highlight in sys.modules, which is what pygments'
#render-time lazy load of the registry entry resolves through. Removing
#it keeps the build rc=0 but silently uncolors every fence (pymdownx
#swallows the import failure via its broad except Exception).
import sys
from pathlib import Path

_SRC = Path(__file__).resolve().parents[1]
if str(_SRC) not in sys.path:
    sys.path.append(str(_SRC))

#Load-bearing, not style: see the self-sufficiency note above (the
#binding itself is unused; the import exists for its sys.modules cache).
from nlang_docs.highlight import NLangLexer  # noqa: E402,F401

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
