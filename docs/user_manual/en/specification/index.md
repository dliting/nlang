# Specification

This tree is the normative specification of the NLang programming
language: its core language, its standard library, and its binary
formats. It defines what a conforming implementation must do. The
[user manual](../index.md) teaches how to use the language and is
informative; where the two disagree about a semantic question, this
specification is the authority. The English text is normative.

The specification describes NLang 0.11.6. It is a living document:
it always describes the current language, and its version history is
the repository history — a tag such as `v0.11.5` pins the whole tree
at one point of the language's evolution.

Where this specification and an implementation disagree, the
disagreement is a defect in one of them. It is resolved by deciding
which side is correct and fixing that side; significant semantic
changes go through the NLang Enhancement Proposal (NLEP) process
before they land here.

## Contents

### Part I — The Language

- [0. Introduction](00-introduction.md)
- [1. Lexical Structure](01-lexical.md)
- [2. Syntax](02-grammar.md)
- [3. Program Structure and Name Resolution](03-program-structure.md)
- [4. Types](04-types.md)
- [5. Expressions](05-expressions.md)
- [6. Statements](06-statements.md)
- [7. Functions and Methods](07-functions.md)
- [8. Object Orientation](08-object-orientation.md)
- 9. Exceptions *(planned)*
- 10. Execution Model and Memory Semantics *(planned)*
- 11. Concurrency *(planned)*
- 12. Packages and Import Search *(planned)*

### Part II — The Standard Library

- 13. Library Conventions *(planned)*
- 14. `std.math` *(planned)*
- 15. `std.io` and Streams *(planned)*
- 16. `std.fs` *(planned)*
- 17. Concurrency Library *(planned)*
- 18. `std.tensor` *(planned)*
- 19. Library Exceptions *(planned)*

### Part III — Binary Formats

- 20. The `.nmod` Module File *(planned)*
- 21. The `.ncu` Compiled Unit *(planned)*
- 22. The `.npkg` Package *(planned)*
- 23. The Native Application Binary Interface (ABI) *(planned)*
