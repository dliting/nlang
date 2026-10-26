# NLEP-0001: Establish the specification system

**Status:** Accepted
**Created:** 2026-10-26
**Spec-Sections:** §0

Record the decision to maintain a normative language specification
alongside the tutorial manual, and the rules under which it evolves.

## Motivation

Before this proposal, NLang's behavior was described only by the
user manual and the implementation. That leaves semantic questions
without an authority: when manual and implementation disagree, there
is nothing to appeal to, and a rule cannot be cited precisely. It
also leaves language evolution undocumented: a change to the type
system or a binary format lands as a code change with no record of
the decision or its alternatives.

## Detailed Semantics

The specification tree at `docs/user_manual/{en,zh}/specification/`
becomes the normative definition of the language, the standard
library and the binary formats, per §0:

- The English text is normative; the Chinese text is a reference
  translation.
- Every numbered heading is a citable clause (`§n.k.m`); automated
  guards reject gaps, duplicates, dangling references and pages
  missing from the site navigation.
- The specification is living: it always describes the current
  language, and the repository history (tags and commits) is the
  version record. There is no compatibility appendix.
- A change to observable semantics is first written up as an NLEP
  in this archive; clarifications and corrections to the text may
  be made directly. Proposals stay editable after acceptance — the
  archive is a living record, not a frozen one.
- The user manual becomes informative: it teaches, links to the
  clauses it summarizes, and no longer carries normative weight.

## Alternatives

- *Frozen, edition-style specification* (the ISO standard process):
  rejected — NLang has no compatibility obligation yet, and an
  edition cycle would freeze bugs as well as features.
- *Specification inside the manual*: rejected — a tutorial cannot
  be the authority without confusing teaching order with normative
  order; the two documents change for different reasons.
- *Immutable proposal archives* (the PEP model): rejected — with no
  backward compatibility, an accepted proposal's text must track
  the living specification, so archives remain editable.

## Consequences

The specification is seeded in three batches: the skeleton and the
lexical and syntax chapters (this proposal's own landing batch),
then the remaining language chapters, then the library and format
chapters with the Chinese translation. The manual gains links into
the clause tree; where the two disagree, the specification wins and
the disagreement is a defect to fix on one side. The guard suite
under `tools/nlang-docs/tests/` grows to keep the specification
synchronized with the lexer, the parser and the site navigation.
