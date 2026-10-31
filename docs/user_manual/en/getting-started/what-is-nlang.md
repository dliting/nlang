# What is NLang?

NLang is a statically-typed scripting language for embedding and
automation — and a testbed for exploring artificial intelligence (AI)-friendly language features.
It ships with its own compiler (ncc), bytecode VM (nvm — it executes the bytecode ncc emits, the instruction form the compiler produces), and integrated development environment nide (IDE),
developed as an open-source teaching/research project.

Getting started: set up the toolchain with [Installation and Layout](install-layout.md),
run your first program with [Your First Program in Five Minutes](first-program.md),
then start a quick tour of the language's core features with runnable
snippets at [Crash Course: Types and Variables](types-and-variables.md).
It is also the landing page for nide's Help menu entry NLang Getting
Started.

Source code (`.n`) compiles first to a bytecode artifact — a `.ncu` unit
image for a single file, a `.npkg` program archive for a multi-file
project — and at run time the VM loads and links the artifact with its
dependencies (the standard-library archive, external modules) into one
runtime module. The language itself:

- **Statically typed** — variables, parameters, and return values
  declare their types explicitly; there is no type inference;
- **Object-oriented** — `class`/inheritance/interfaces/virtual dispatch,
  plus struct (value types) and enum;
- **Built-in generic collections** — `List<T>`/`Dict<K,V>` with
  initializer syntax;
- **Garbage-collected** — no `delete`; objects are reclaimed
  automatically once unreachable, so you never free memory by hand (see
  [Memory Management](../language-spec/memory-management.md));
- **Concurrent** — `thread.spawn` runs a function on its own thread and
  yields a `Future<T>` for its result; the `concurrent` package
  (`Mutex`, `Channel`, `Semaphore`, `WaitGroup`) provides the
  synchronization layer;
- **Modern scripting staples** — exception handling, string
  interpolation, `out` parameters, default parameters, and the
  `math`/`io`/`fs`/`thread`/`concurrent` standard libraries.

The six tools shipped with the package:

| Tool | Role |
|---|---|
| nlang | script runner: runs `.n` scripts or `-c` inline statements without the `main()` boilerplate — compiles and executes in-process, leaving no artifact on disk |
| ncc | command-line compiler: compiles `.n` source files into `.ncu` and `.nproj` project files into `.npkg`, and can execute right after compiling |
| nvm | VM runner: executes `.ncu` and `.npkg` |
| ndb | command-line debugger: breakpoints, stepping, and the call stack (it also drives the graphical debugging in nide) |
| ndisasm | bytecode disassembler: inspects the instructions inside a `.ncu`/`.npkg` |
| nide | IDE: edit, build, and run, with this help site embedded |

For the complete usage of the six tools, see the
[Command-line Tools](../cli-tools/overview.md) chapter; for nide's
graphical interface, see [Three Ways to Run](running.md) and
[Debugging in nide](debugging.md).
