# Specification

This tree is the normative specification of the NLang programming
language: its core language, its standard library, and its binary
formats. It defines what a conforming implementation must do. The
[user manual](../index.md) teaches how to use the language and is
informative; where the two disagree about a semantic question, this
specification is the authority. The English text is normative.

The specification describes NLang 0.12.3. It is a living document:
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
- [3. Program Structure and Name Resolution](03-program-structure/index.md)
  - [3.8 Declarations](03-program-structure/declarations.md)
  - [3.9 Packages and Import Search](03-program-structure/packages.md)
- [4. Types](04-types/index.md)
  - [4.2 Primitive Types](04-types/primitives.md)
  - [4.3 String](04-types/string.md)
  - [4.5 Type Casts](04-types/casts.md)
  - [4.6 Enum](04-types/enum.md)
  - [4.7 Struct](04-types/struct.md)
  - [4.8 Class](04-types/class.md)
  - [4.9 Interface](04-types/interface.md)
  - [4.10 Array](04-types/array.md)
  - [4.11 Object and Boxing](04-types/object.md)
  - [4.12 Generics](04-types/generics.md)
- [5. Expressions](05-expressions/index.md)
  - [5.10 Operators](05-expressions/operators.md)
  - [5.11 Collection Initializers](05-expressions/initializers.md)
- [6. Statements](06-statements/index.md)
  - [6.7 Control Flow](06-statements/control-flow.md)
  - [6.8 Loops](06-statements/loops.md)
- [7. Functions](07-functions/index.md)
  - [7.2 Parameters and Arguments](07-functions/parameters.md)
  - [7.3 Overload Resolution](07-functions/overloads.md)
  - [7.4 Function Values and Delegates](07-functions/function-values.md)
  - [7.5 Native Functions](07-functions/native.md)
- [8. Exceptions](08-exceptions/index.md)
- [9. Execution Model](09-execution-model/index.md)
  - [9.2 Lifetime and Garbage Collection](09-execution-model/memory.md)
  - [9.3 Program Entry and Exit](09-execution-model/exit-codes.md)
- [10. Concurrency](10-concurrency/index.md)
  - [10.8 Carriers and Parallelism](10-concurrency/carriers.md)
  - [10.9 Suspension and Scheduling Limits](10-concurrency/suspension.md)

### Part II — The Standard Library

- [13. Built-in Classes and Library Conventions](13-builtin-classes.md)
- [14. The math Package](14-math.md)
- [15. The io Package and Streams](15-io-streams.md)
- [16. The fs Package](16-fs.md)
- [17. The Concurrency Library](17-concurrency-library.md)
- [18. The tensor Package](18-tensor.md)
- [19. Library Exceptions](19-library-exceptions.md)

### Part III — Binary Formats

- [20. The `.ncu` Unit Image](20-ncu-unit-image.md)
- [21. The `.npkg` Package Archive](21-npkg-package-archive.md)
- [22. The Native Application Binary Interface (ABI)](22-native-abi.md)
