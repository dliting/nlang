# Changelog

All notable changes to NLang are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

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

[0.2.0]: https://github.com/dliting/nlang/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/dliting/nlang/releases/tag/v0.1.0
