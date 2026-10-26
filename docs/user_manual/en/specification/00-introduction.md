# 0. Introduction

## 0.1 Scope and Status

This specification is the normative definition of the NLang
programming language. It covers three parts: the core language
(Part I), the standard library (Part II), and the binary formats an
implementation produces and consumes (Part III).

It is a living document. It describes the current language and
carries no compatibility appendix: when the language changes, the
text changes with it, and the repository history (tags and commits)
is the version record. A change to observable semantics — a new or
removed feature, a type-system rule, a format change, a library
interface addition or removal — is first written up as an NLang
Enhancement Proposal (NLEP); clarifications and corrections to the
text may be made directly.

This specification is the authority for semantic questions. The
user manual is a tutorial and is informative; it links here for the
rules it summarizes.

## 0.2 Normative Language

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY in this
specification are to be interpreted as described in RFC 8174. A
statement marked informative is guidance, not a requirement. Where
this specification is silent, the behavior is unspecified: an
implementation MAY choose any behavior, but MUST NOT document a
different one as conforming.

A rule that rejects a program before it runs is stated as "It is a
compile-time error if …". A rejected program MUST be diagnosed; an
implementation MUST NOT silently execute it. A rule about behavior
during execution states the observable outcome, including which
exception is thrown when one is thrown.

## 0.3 Clause References

Every numbered heading in this specification is a clause, cited as
`§n`, `§n.k` or `§n.k.m`. Clause numbers are unique across the
tree, dense within a chapter, and stable: automated guards reject
gaps, duplicates and dangling references. Citing a clause by number
is valid from the user manual, from changelog entries and from
NLEPs; those citations name the clause by number only, never by a
path into this repository.

## 0.4 Terms and Definitions

A defined term is set in italics at its first definition and linked
from later uses.

*Compilation unit*: one `.n` source file as processed by the
compiler, together with the declarations it imports (see §12 when
written).

*Normative*: binding on implementations. The English text of this
specification is normative; translations are informative.

*Informative*: guidance and explanation that implementations are
free to ignore.

*Unspecified*: the specification imposes no requirement; different
implementations, or different runs of one implementation, MAY
differ.

*Implementation-defined*: the implementation chooses a behavior, but
MUST document it and MUST NOT contradict the rest of this
specification.

## 0.5 Conformance

An implementation conforms if the observable behavior of every
program it accepts matches this specification: the programs it
accepts and rejects, the diagnostics it is required to produce, the
values and exceptions a program can observe, and the permitted
interleavings of concurrent execution.

What happens inside an implementation is outside this
specification. The virtual machine's internal representation, its
register allocation, the optimizations it performs and the internal
structure of its garbage collector are not constrained here,
provided every observable outcome still matches §10 when written.
The binary formats of Part III are the exception: they are specified
because tools and host programs consume them directly.
