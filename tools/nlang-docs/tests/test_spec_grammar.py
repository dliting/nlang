"""Grammar name coverage, both directions: every named nonterminal in
nlang.y's rule section appears as a defined symbol in the §2 EBNF, and
every EBNF definition corresponds to a parser rule or a documented
spec-side grouping. Production bodies are reviewed by hand (naming
noise and bison-only disambiguation make body-level diffing useless);
this guard catches whole-rule drift in either direction."""
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
#  TypeParamRests -- the spec's TypeParams writes the comma list
#  inline ("<" <identifier> { "," <identifier> } ">"); bison factors
#  the zero-or-more tail into its own nonterminal.
EXEMPT = {"AccessTypeEmpty", "AccessTypeNonEmpty", "NodeFlagsNonEmpty",
          "TypeParamRests"}
# EBNF-only groupings: the spec factors these symbols out of parser
# rules that inline the alternatives at their use sites; each entry
# names where the parser spells it:
#  BinaryOp / UnaryOp / CompoundOp -- operator token groups; bison
#  inlines the OT_ alternatives into Expression and into the
#  compound-assignment and for-fini arms.
#  FieldDecl / MethodDecl / ConstDecl -- bison inlines the member
#  shapes into ClassMember and InterfaceMember.
#  LocalDecl -- bison inlines the per-declarator arms into
#  LocalDeclList.
#  TypeArgs -- bison inlines the "<...>" bracket forms (including the
#  KT_Void variants) into Type, HeadType and the func-type rules.
#  TypeKeyword -- bison inlines the KT_ type-keyword alternatives
#  into IdentifierExpr.
#  NodeFlags -- the spec's one-or-more flag list; bison spells the
#  non-empty list NodeFlagsNonEmpty (the empty layer was removed in
#  v0.9.1) and writes optionality as [ NodeFlags ].
SPEC_ONLY = {"BinaryOp", "UnaryOp", "CompoundOp", "FieldDecl", "MethodDecl",
             "ConstDecl", "LocalDecl", "TypeArgs", "TypeKeyword", "NodeFlags"}


def _nonterminals():
    sections = PARSER.read_text(encoding="utf-8", errors="replace").split("\n%%\n")
    return {m.group(1) for m in LHS.finditer(sections[1])}


def _ebnf_defined():
    return set(EBNF_DEF.findall(SPEC_GRAMMAR.read_text(encoding="utf-8")))


def test_every_nonterminal_defined():
    nonterms = _nonterminals()
    assert nonterms, "parser rule section came up empty"
    missing = nonterms - _ebnf_defined() - EXEMPT
    assert not missing, f"nonterminals without EBNF rule: {sorted(missing)}"


def test_every_ebnf_rule_exists():
    # the reverse direction: a production renamed or removed in the
    # parser would otherwise linger in the spec as a stale definition
    defined = _ebnf_defined()
    assert defined, "the EBNF section came up empty"
    stale = defined - _nonterminals() - SPEC_ONLY
    assert not stale, f"EBNF rules with no parser counterpart: {sorted(stale)}"
