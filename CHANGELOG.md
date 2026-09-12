# Changelog

All notable changes to NLang are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [0.6.0] - 2026-09-12

### Added

- Array-valued expressions carry a resolve-time type property; jagged
  array declarations (`T[][]`) and non-container `foreach` sources are
  rejected at compile time with named diagnostics.
- GC tracks array records held in array-typed element slots.

### Changed

- `.nmod` format floor raised to v1.10: array struct/class fields now
  store `RTK_Array` as their field kind (previously the element kind);
  older modules must be recompiled.
- Streaming a struct with an array field (`bs.writeStruct`) now throws
  a named error instead of silently writing the raw heap handle.

### Fixed

- Class fields typed `int[]` no longer break `toString()` dispatch.
- `.length` resolves on any array-valued receiver (`li.get(0).length`,
  `lib.mk(3).length`), not just identifier locals/fields.

## [0.5.0] - 2026-09-09

### Added

- nide: a debugging suite. F5 starts a session — the program builds,
  then runs to the first breakpoint or to completion — and pressing F5
  again continues; Shift+F5 stops the session at any time (a hard
  terminate that always works, including inside infinite loops or
  native code). F9 or a gutter click toggles a breakpoint (filled dot =
  bound in the live session, hollow = not bound); breakpoints persist
  across restarts and follow file renames. F10/F11/Shift+F11 step
  over/into/out. The new 调试 page in the output area shows the session
  status, a 抛异常时中断 (break-on-throw) switch, the call stack
  (clicking a frame selects it, jumps to the line and refreshes locals)
  and the selected frame's locals; the paused line is highlighted in
  the editor with a gutter arrow. Program output and error backtraces
  stream to the 运行输出 page. Build/Run are disabled while a session
  is live, and closing nide terminates the debugged process.
- VM: `IHostIo` — a host I/O seam for embedded front ends: output bytes
  arrive verbatim through a callback, and an installed host that
  declares no input makes `io.readLine` raise a catchable IOException
  instead of silently consuming the embedder's stream. With no host
  installed (the default) the console behavior is unchanged, so ncc,
  nvm and the CLI debugger are unaffected.
- ndb: `--machine` mode — a line protocol over stdin/stdout for IDE
  embedding (tab-joined events with escaped fields:
  hello/bp/stopped/frame/local/done/output/exited/error/err; breakpoint
  setup before `run`).

### Changed

- VM: while/for/do-while line breakpoints and stepping now hit on every
  iteration — the back edge lands on the anchor (condition entry for
  while/for, tail condition for do-while; gdb semantics). Previously
  the anchor fired only at loop entry, so an empty-body loop had no
  per-iteration checkpoint.
- ndb: breakpoint identity is now one id per source line — a line
  carrying several statement anchors (e.g. a loop header) merges them
  under a single breakpoint id, so setting, deleting and reporting
  breakpoints behave identically in the CLI and machine mode.
- nide: 运行 → 开始运行 moved from F5 to Ctrl+F5; F5 now starts (and
  continues) the debugger.

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
