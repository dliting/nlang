# Overview


NLang is a statically-typed scripting language with C-like syntax. It
compiles to bytecode (intermediate instructions executed by a virtual
machine) and is designed for embedding and automation — a testbed for
artificial intelligence (AI)-friendly language features.

Key design goals:
- Familiar C-family syntax for low learning curve
- Value semantics for structs, reference semantics for classes (like C#)
- Deterministic, inspectable memory layout (every value has a definite place on the heap and an explicit type tag)
- Mark-sweep garbage collection for class objects
- Embeddable in host applications via a small C++ application programming interface (API)

### How to read this specification

- [Types](types.md) — the type system at a glance; enum, struct, class, interface, array, and the rest each have their own page
- [Declarations](declarations.md) and [Packages](packages.md) — imports, variables, type aliases, and module visibility
- [Expressions & Statements](expressions.md), [Functions](functions.md)
- [Concurrency](concurrency.md) — virtual threads, channels, mutexes, semaphores, and futures
- [Standard Library](standard-library.md)
- Runtime: [Memory Management](memory-management.md) and the [Exit Code Convention](exit-code-convention.md)
- Reference: [Known Limitations](known-limitations.md) and [Common Error Messages](common-errors.md)

For a suggested reading order, see [Further Reading](../getting-started/further-reading.md).
