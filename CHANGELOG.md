# Changelog

All notable changes to NLang are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [0.5.0] - In development

### Added

- ndb: `--machine` mode — a line protocol over stdin/stdout for IDE
  embedding (tab-joined events with escaped fields:
  hello/bp/stopped/frame/local/done/output/exited/error/err; breakpoint
  setup before `run`).

### Changed

- VM: while/for/do-while line breakpoints and stepping now hit on every
  iteration — the back edge lands on the anchor (condition entry for
  while/for, tail condition for do-while; gdb semantics). Previously
  the anchor fired only at loop entry, so an empty-body loop had no
  per-iteration checkpoint. (ndb/nide debugging work in progress —
  this section will grow.)

## [0.4.0] - 2026-09-07

### Added

- ndb: a CLI debugger for compiled modules. Breakpoints
  (`b <file.n:LINE | LINE | funcName>`), continue, step into/over/out,
  backtrace, frame selection, `info locals`, `p`, source listing `l`,
  disassembly `x`, `catch on|off` (break on throw). Initial stop at the
  first statement (like gdb `start`); stdin EOF behaves like `q`; ndb
  exits with the debugged program's exit code.
- VM: in-process debug hooks (`IDebugHooks` — statement and throw
  checkpoints) plus a read-only frozen-state view (`IVmDebugView`);
  front-end-agnostic interfaces a future DAP adapter or the IDE can
  reuse. Disassembly printing extracted into a shared `Disassembler`
  (ndisasm output byte-identical).
- `.nmod` v1.9: each function records its source file path (cross-file
  breakpoint addressing). The import merge now also copies
  `func.locals`, fixing a pre-existing GC root-set hole where imported
  frames had an empty root set (live objects could be swept).

### Changed

- `.nmod` format floor raised from 8 to 9: older modules are refused
  by the loader and must be recompiled.

## [0.3.0] - 2026-08-31

### Added

- nide: the About dialog now shows the project's GitHub address
  (https://github.com/dliting/nlang) as a blue underlined link; clicking
  it opens the default browser.

### Changed

- Language: `&&` and `||` now short-circuit (the skipped operand is never
  evaluated — no side effects, no throws), matching C/C++/Java/Python
  conventions; results stay `int` `0`/`1`. Operands of `&&`, `||` and `!`
  must now be `int` (float/string operands were previously read as raw
  bits with meaningless truthiness — now a compile error). Old bytecode
  modules containing the removed eager `OP_LogicalAnd`/`OP_LogicalOr`
  instructions must be recompiled.

## [0.2.0] - 2026-08-31

### Added

- nide: **File > Recent** submenu — recent solutions, projects and files,
  most-recently-used first, persisted across sessions. Entries with the same
  file name are disambiguated by parent directory and carry full-path
  tooltips; opening, creating, save-as and rename all feed the list.
- Version management: the repository-root `VERSION` file is now the single
  source of the version — `ncc`/`nvm`/`ndisasm --version`, the IDE About
  dialog, the documentation-site footer and package names all derive from
  it. This changelog is part of that workflow.

## [0.1.0] - 2026-08-30

First public release.

### Added

- Language: a statically-typed scripting language — classes and structs
  with inheritance and `super()`, functions, methods and delegates, type
  aliases, arrays, `List`/`Dict`, `foreach`, `switch`/`enum`, exception
  handling (`try`/`catch`/`finally`/`throw` with built-in exception
  classes), string interpolation, incremental assignment, `assert` and
  `const` locals.
- Toolchain: `ncc` (compile and run; single files and `.nproj` projects),
  `nvm` (bytecode runner), `ndisasm` (bytecode disassembler).
- nide: a Qt5 IDE — solution tree with standalone files, editor, build and
  run, embedded offline documentation viewer, Chinese/English UI.
- Standard library: `math`/`io`/`fs` namespaces and built-in string
  methods.
- Cross-file programming: explicit `import` (single, wildcard and
  precompiled `.nmod` module forms).
- Documentation: a fully offline-capable documentation site and runnable
  `examples/`.
- Windows packaging: portable zip and NSIS installer.

[0.5.0]: https://github.com/dliting/nlang/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/dliting/nlang/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/dliting/nlang/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/dliting/nlang/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/dliting/nlang/releases/tag/v0.1.0
