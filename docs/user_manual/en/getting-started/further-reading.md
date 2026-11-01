# Further Reading

Suggested reading order through the
[Specification](../specification/index.md):

1. [Types](../specification/04-types/index.md) →
   [Declarations](../specification/03-program-structure/declarations.md) →
   [Generics](../specification/04-types/generics.md)
   — the type system, declarations, and List/Dict
2. [Expressions](../specification/05-expressions/index.md) →
   [Statements](../specification/06-statements/index.md)
   — operators, control flow, exceptions
3. [Functions](../specification/07-functions/index.md) →
   [Function Values and Delegates](../specification/07-functions/function-values.md)
4. [Concurrency](../specification/10-concurrency/index.md) — spawning
   virtual threads with `thread.spawn`, channels, mutexes, and Future
5. [Library Conventions](../specification/library/11-conventions.md)
   — the standard library at a glance, followed by the package chapters
6. [Lifetime and Garbage Collection](../specification/09-execution-model/memory.md)
   and [Program Entry and Exit](../specification/09-execution-model/exit-codes.md)

The reference pages — [Naming Convention](../language-spec/naming-convention.md),
[Known Limitations](../language-spec/known-limitations.md), and
[Common Error Messages](../language-spec/common-errors.md) — are
informative companions to the specification.

For the execution engine: start with
[VM Architecture / Overview](../vm-architecture/overview.md), then read
the compilation pipeline, stack frame layout, bytecode instructions, and
the other chapters as needed. The five command-line tools (nlang, ncc,
nvm, ndb, ndisasm) are documented in the
[Command-line Tools](../cli-tools/overview.md) chapter. For debugging
inside the integrated development environment (IDE), see [Debugging in nide](debugging.md).

For complete runnable programs: `examples/README.md` lists every example
by topic with its expected exit code, and most of this crash course's
snippets have a corresponding example there.

To extend or embed: [Developing Libraries](../libraries/developing-libraries.md) shows how to package reusable code as a library (including native implementations), and [Integrating NLang](../libraries/integrating-nlang.md) how to drive NLang from your own program via subprocesses — dependencies, deployment and debugging.
