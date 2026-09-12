# -*- coding: utf-8 -*-
"""Forbidden-public-text patterns for NLang.

NLang ships with zero references to its private predecessor codebase:
neither narrative mentions nor comment-style lineage references. This
module is the single pattern source — the repo-level pytest gate
(tools/nlang-docs/tests/test_public_text.py) and the release-package
gate (tests/packaging/verify_package.py) both scan with it, so the two
gates cannot drift apart.

Callers read files as bytes themselves (binary detection is policy):
feed decoded text to find_violations().

Legal neighbors these patterns deliberately leave alone: the word
"engine" alone and the VM's "execution engine" sense of 引擎, "English",
lowercase locale codes like en-US, and "reference" in ordinary prose.
"""
import re

#(name, regex) — name appears in violation messages.
FORBIDDEN_PATTERNS = [
    #Chinese narrative forms ("inspired by EN", "EN game engine") and
    #the bare term regardless of qualifier.
    ("zh-en-inspired", re.compile(r"受\s*EN")),
    ("zh-en-engine", re.compile(r"EN\s*(?:游戏引擎|引擎)")),
    ("zh-game-engine", re.compile("游戏引擎")),
    ("en-game-engine", re.compile(r"game\s+engine", re.IGNORECASE)),
    #Case-sensitive standalone "EN" (word boundary keeps "English",
    #"children", "en-US" legal); predecessor backup dirs and paths.
    ("en-word", re.compile(r"\bEN\b")),
    ("legacy-backup-dirs", re.compile(r"compiler_bak|lang_bak")),
    #Windows paths are case-insensitive; the trailing \b keeps
    #"cases/energy"-style words legal.
    ("legacy-repo-path", re.compile(r"cases[/\\]en\b", re.IGNORECASE)),
    #Predecessor mechanism names that have no counterpart in this tree
    #(specific proper nouns — no false-positive risk).
    ("legacy-identifiers",
     re.compile(r"I_Base_|FixChainedJumps|SeStatements|SeBasics|"
                r"SeExpressions")),
]

#Violation excerpts are clipped so one long minified line (a packaged
#search index, say) cannot flood the failure output.
_EXCERPT_MAX = 80


def find_violations(text):
    """Return [(pattern_name, line_no, excerpt)] for every match.

    Line numbers are 1-based; empty when the text is clean.
    """
    violations = []
    for name, pattern in FORBIDDEN_PATTERNS:
        for match in pattern.finditer(text):
            line_no = text.count("\n", 0, match.start()) + 1
            line_start = text.rfind("\n", 0, match.start()) + 1
            excerpt = text[line_start:text.find("\n", line_start)]
            if len(excerpt) > _EXCERPT_MAX:
                excerpt = excerpt[:_EXCERPT_MAX] + "..."
            violations.append((name, line_no, excerpt.strip()))
    return violations
