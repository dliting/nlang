# Overview


NLang is a statically-typed scripting language with C-like syntax. It
compiles to bytecode executed by a register-based VM, and is designed for
embedding and automation — a testbed for AI-friendly language features.

Key design goals:
- Familiar C-family syntax for low learning curve
- Value semantics for structs, reference semantics for classes (like C#)
- Deterministic, inspectable memory layout (heap slots, typed kinds)
- Mark-sweep garbage collection for class objects
- Embeddable in host applications via a small C++ API
