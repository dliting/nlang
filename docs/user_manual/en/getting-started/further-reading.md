# Further Reading

Suggested reading order for the Language Specification:

1. [Overview](../language-spec/overview.md) — what the language is and
   how it is organized
2. [Naming Convention](../language-spec/naming-convention.md) — the
   naming rules for types/methods/variables
3. [Types](../language-spec/types.md) →
   [Type Semantics](../language-spec/type-semantics.md) →
   [Declarations](../language-spec/declarations.md) →
   [Built-in Generic Classes](../language-spec/builtin-generic-classes.md)
   — the type system, declarations, and List/Dict
4. [Expressions](../language-spec/expressions.md) →
   [Statements](../language-spec/statements.md)
   — operators, control flow, exceptions
5. [Functions](../language-spec/functions.md) →
   [Function Types & Delegates](../language-spec/function-types-and-delegates.md)
6. [Standard Library](../language-spec/standard-library.md),
   [Memory Management](../language-spec/memory-management.md),
   [Exit Code Convention](../language-spec/exit-code-convention.md),
   [Known Limitations](../language-spec/known-limitations.md)

For the execution engine: start with
[VM Architecture / Overview](../vm-architecture/overview.md), then read
the compilation pipeline, stack frame layout, bytecode instructions, and
the other chapters as needed. The four command-line tools (ncc, nvm, ndb,
ndisasm) are documented in the
[Command-line Tools](../cli-tools/overview.md) chapter. For debugging
inside the IDE see [Debugging in nide](debugging.md).

For complete runnable programs: `examples/README.md` lists every example
by topic with its expected exit code, and most of this crash course's
snippets have a corresponding example there.
