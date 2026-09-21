# What is NLang?

NLang is a statically-typed scripting language for embedding and
automation — and a testbed for exploring AI-friendly language features.
It ships with its own compiler (ncc), bytecode VM (nvm), and IDE (nide),
developed as an open-source teaching/research project.

This page walks you through installation, running your first program,
and a quick tour of the language's core features with runnable snippets.
It is also the landing page for nide's Help menu entry NLang Getting
Started.

Source code (`.n`) compiles first to a bytecode module (`.nmod`), which
the VM then executes. The language itself:

- **Statically typed** — variables, parameters, and return values
  declare their types explicitly; there is no type inference;
- **Object-oriented** — `class`/`extends`/interfaces/virtual dispatch,
  plus struct (value types) and enum;
- **Built-in generic collections** — `List<T>`/`Dict<K,V>` with
  initializer syntax;
- **Modern scripting staples** — exception handling, string
  interpolation, `out` parameters, default parameters, and the
  `math`/`io`/`fs` standard libraries.

The five tools shipped with the package:

| Tool | Role |
|---|---|
| ncc | command-line compiler: compiles `.n`/`.nproj` into `.nmod`, and can execute right after compiling |
| nvm | VM runner: executes `.nmod` |
| ndb | command-line debugger: breakpoints, stepping, and the call stack (it also drives the graphical debugging in nide) |
| ndisasm | bytecode disassembler: inspects the instructions inside a `.nmod` |
| nide | IDE: edit, build, and run, with this help site embedded |

For the complete usage of the five tools, see the
[Command-line Tools](../cli-tools/overview.md) chapter; for nide's
graphical interface, see [Three Ways to Run](running.md) and
[Debugging in nide](debugging.md).
