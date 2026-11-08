# Specification

This tree is the normative specification of the NLang programming
language: its core language, its standard library, and its binary
formats. It defines what a conforming implementation must do, and
each chapter pairs the normative rules with worked examples and
error guidance. For a guided introduction to the language, see
[Getting Started](../getting-started/what-is-nlang.md); it is
informative and links here for the rules it summarizes. The English
text is normative.

The specification describes NLang 0.13.4. It is a living document:
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

- [11. Library Conventions](library/11-conventions.md)
- [12. Collections](library/12-collections.md)
- [13. The math Package](library/13-math.md)
- [14. The io Package and Streams](library/14-io/index.md)
  - [14.6 ByteStream and FileStream](library/14-io/streams.md)
  - [14.8 Base64](library/14-io/base64.md)
- [15. The system Package](library/15-system.md)
- [16. The fs Package](library/16-fs.md)
- [17. string Methods](library/17-string-methods.md)
- [18. The Concurrency Classes](library/18-concurrency-classes/index.md)
  - [18.3 The thread Package](library/18-concurrency-classes/thread.md)
  - [18.4 Future](library/18-concurrency-classes/future.md)
  - [18.5 Promise](library/18-concurrency-classes/promise.md)
  - [18.6 Mutex](library/18-concurrency-classes/mutex.md)
  - [18.7 Semaphore](library/18-concurrency-classes/semaphore.md)
  - [18.8 ReentrantMutex](library/18-concurrency-classes/reentrant-mutex.md)
  - [18.9 AtomicInt](library/18-concurrency-classes/atomic.md)
  - [18.10 Channel](library/18-concurrency-classes/channel.md)
  - [18.11 The concurrent Package](library/18-concurrency-classes/concurrent.md)
- [19. The tensor Package](library/19-tensor.md)
- [20. Library Exceptions](library/20-library-exceptions.md)
- [21. The Network Library](library/21-network/index.md)
  - [21.2 The Transmission Control Protocol (TCP) Classes](library/21-network/tcp.md)
  - [21.3 The UdpSocket Class](library/21-network/udp.md)
  - [21.4 The Internet Control Message Protocol (ICMP) Surface](library/21-network/icmp.md)
- [22. The url Package](library/22-url.md)

### Part III — Binary Formats

- [23. The `.ncu` Unit Image](formats/23-ncu.md)
- [24. The `.npkg` Package Archive](formats/24-npkg.md)
- [25. The Native Application Binary Interface (ABI)](formats/25-abi.md)

### Reference

- [Known Limitations](reference/known-limitations.md)
- [Common Error Messages](reference/common-errors.md)
- [Naming Convention](reference/naming-convention.md)
