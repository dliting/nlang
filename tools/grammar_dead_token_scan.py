# Scan nlang.y for tokens that are declared (or produced by the lexer) but
# never consumed by any grammar production, and nlang.l rules emitting tokens
# the grammar never uses. Guards the v0.13.7 half-finished-scaffolding audit:
# a token the lexer emits and the grammar declares but no production consumes
# reserves its spelling (or its operator sequence) for no feature.
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
YE = REPO / "src" / "compiler" / "grammar" / "nlang.y"
LE = REPO / "src" / "compiler" / "grammar" / "nlang.l"

ytext = open(YE, encoding="utf-8", errors="replace").read()
ltext = open(LE, encoding="utf-8", errors="replace").read()

# Strip comments so commented-out precedences don't count as "used".
ycode = re.sub(r"/\*.*?\*/", "", ytext, flags=re.S)
ycode = re.sub(r"//[^\n]*", "", ycode)

# Declared token names: %token / %token <type> lines
decl_tokens = set()
for m in re.finditer(r"^%(?:token|token-type|type)[^\n]*", ycode, flags=re.M):
    for name in re.findall(r"\b(OT_[A-Za-z0-9_]+|KT_[A-Za-z0-9_]+|NK_[A-Za-z0-9_]+)\b", m.group(0)):
        decl_tokens.add(name)
# Single-quoted char terminals declared in %left/%right/%nonassoc lines
decl_chars = set(re.findall(r"'(.)'", ycode))

# Usages: production bodies (lines with ':' or continuation) + precedence decls count as use
# Count token occurrences outside the %token declaration lines themselves.
body = re.sub(r"^%(?:token|token-type|type)[^\n]*", "", ycode, flags=re.M)

dead_tokens = []
for tok in sorted(decl_tokens):
    uses = len(re.findall(r"\b%s\b" % re.escape(tok), body))
    if uses == 0:
        dead_tokens.append(tok)

# Lexer rules that emit a token the grammar body never mentions
lex_emitted = set(re.findall(r"\b(OT_[A-Za-z0-9_]+|KT_[A-Za-z0-9_]+)\b", ltext))
lex_dead = []
for tok in sorted(lex_emitted):
    uses = len(re.findall(r"\b%s\b" % re.escape(tok), body))
    if uses == 0:
        lex_dead.append(tok)

print("declared-but-unused tokens:")
for t in dead_tokens:
    print("  " + t)
print("lexer-emitted-but-grammar-unused tokens:")
for t in lex_dead:
    print("  " + t)
print("single-quoted char terminals in precedence lines:")
print("  " + " ".join(sorted(decl_chars)))
# Which single chars appear in productions at all?
chars_in_productions = set(re.findall(r"^\s*(?:[a-zA-Z_][\w]*\s*:|.*\|)\s*.*?'(.)'", body, flags=re.M))
print("char terminals seen in production bodies:", " ".join(sorted(chars_in_productions)))
# Non-zero exit when either dead column is non-empty: keeps the scan usable
# as a gate, not just a report.
raise SystemExit(1 if dead_tokens or lex_dead else 0)
