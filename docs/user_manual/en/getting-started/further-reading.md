# Further Reading

Suggested reading order for the Language Specification:

1. [Overview](../language-spec/overview.md) — what the language is and
   how it is organized
2. [Naming Convention](../language-spec/naming-convention.md) — the
   naming rules for types/methods/variables
3. [Types](../specification/04-types/index.md) →
   [Declarations](../specification/03-program-structure/declarations.md) →
   [Generics](../specification/04-types/generics.md)
   — the type system, declarations, and List/Dict
4. [Expressions](../specification/05-expressions/index.md) →
   [Statements](../specification/06-statements/index.md)
   — operators, control flow, exceptions
5. [Functions](../specification/07-functions/index.md) →
   [Function Values and Delegates](../specification/07-functions/function-values.md)
6. [Concurrency](../specification/10-concurrency/index.md) — spawning
   virtual threads with `thread.spawn`, channels, mutexes, and Future
7. [Standard Library](../language-spec/standard-library.md),
   [Lifetime and Garbage Collection](../specification/09-execution-model/memory.md),
   [Program Entry and Exit](../specification/09-execution-model/exit-codes.md),
   [Known Limitations](../language-spec/known-limitations.md)

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
