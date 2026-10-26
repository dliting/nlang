"""Grammar name coverage: every named nonterminal in nlang.y's rule
section appears as a defined symbol in the §2 EBNF. Production bodies
are reviewed by hand (naming noise and bison-only disambiguation make
body-level diffing useless); this guard catches whole-rule drift."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PARSER = REPO / "src" / "compiler" / "grammar" / "nlang.y"
SPEC_GRAMMAR = (REPO / "docs" / "user_manual" / "en" / "specification"
                / "02-grammar.md")

# Bison rule LHS at column 0 inside the rule section (between the first
# and second bare %% lines):  name:  -- C++ action code is indented, so
# column-0 anchors separate rule heads from action statements.
LHS = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\s*:", re.M)
# EBNF defined symbols:  name = ... ;  at line start
EBNF_DEF = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\s*=", re.M)
# bison-internal helpers the spec folds into one rule each; every entry
# needs a comment why it earns an exemption:
#  AccessTypeEmpty / AccessTypeNonEmpty -- the spec's AccessType is the
#  non-empty word list; optionality is written [ AccessType ]. Bison
#  splits the two to keep the empty case a distinct LALR symbol.
#  NodeFlagsNonEmpty -- the spec writes NodeFlags (one or more); the
#  NonEmpty suffix only pairs the list against single-flag positions.
EXEMPT = {"AccessTypeEmpty", "AccessTypeNonEmpty", "NodeFlagsNonEmpty"}


def _nonterminals():
    sections = PARSER.read_text(encoding="utf-8", errors="replace").split("\n%%\n")
    return {m.group(1) for m in LHS.finditer(sections[1])}


def test_every_nonterminal_defined():
    nonterms = _nonterminals()
    assert nonterms, "parser rule section came up empty"
    defined = set(EBNF_DEF.findall(
        SPEC_GRAMMAR.read_text(encoding="utf-8")))
    missing = nonterms - defined - EXEMPT
    assert not missing, f"nonterminals without EBNF rule: {sorted(missing)}"
