# Contributing to NLang

NLang is a teaching/research project — a statically-typed scripting language with
its own compiler front end, bytecode VM and Qt IDE. Contributions are welcome,
especially ones that come with a small design note: language-semantics changes
are decided before they are coded (open an issue first).

## Getting Started

Build prerequisites and platform setup live in the
[README](README.md#build-dependencies). The short version:

```bash
cmake -B build -DNLANG_BUILD_TESTS=ON \
    -DFLEX_EXE=<path-to-flex> -DBISON_EXE=<path-to-bison>
cmake --build build --config Release
```

Two Windows-specific traps worth knowing up front:

- `NLANG_BUILD_TESTS` defaults to **OFF** — without it ctest reports
  "No tests were found" and still exits 0. Always configure it explicitly and
  check the test count.
- Flex/Bison are not auto-discovered reliably; pass `FLEX_EXE`/`BISON_EXE`
  explicitly ([win_flex_bison](https://github.com/lexxmark/winflexbison) on
  Windows).

## Testing

A change is done when the suites below pass, not when it compiles.

| Suite | Command | What it covers |
|---|---|---|
| Unit tests | `ctest --test-dir build -C Release` | Compiler, VM, runtime, IDE logic |
| End-to-end | `python tests/e2e/run_e2e_tests.py <ncc> <nvm>` | Real `.n` programs, real bytecode execution (`tests/e2e/manifest.txt`) |
| Docs pipeline | `pytest tools/nlang-docs/tests` | Docs-site build tooling |
| Packaging | `python tests/packaging/verify_package.py <release_dir>` | Release zip/installer layout + smoke run |

The e2e runner takes ncc/nvm paths as positional arguments and defaults to the
`build/` tree — pass explicit paths when testing an IDE (`build-ide/`) build.

Tests should exercise real behavior (compile real `.n` files, execute real
bytecode), not mocks of the compiler.

## Release Process

The version number lives in one place: the repository root `VERSION` file.
CMake reads it at configure time and feeds the tools' `--version`, the IDE's
About dialog, the documentation-site footer and the CPack package names —
bumping the file is the only version edit a release needs.

1. Edit `VERSION` (semantic versioning, e.g. `0.2.0`).
2. Rebuild and run the gates — ctest includes a guard that every tool's
   `--version` echoes the file, and `verify_package.py` checks the package
   name against it.
3. `git tag -a v<version> -m "NLang <version>"` and push the tag.
4. Package and upload as described in [Packaging](README.md#packaging-windows).

## Code Style

- C++17; everything lives in the `nlang::` namespace.
- Naming: types `PascalCase`; functions, methods and variables `camelCase`
  (`main` excepted). `getXxx` names are reserved for future property syntax.
  Prefer specific names over generic ones (`cache`, `result`, `data` are
  suspect).
- Memory: `std::unique_ptr` / `std::shared_ptr` for ownership. Member prefixes:
  `m_up*` (unique_ptr), `m_sp*` (shared_ptr), `m_p*` (non-owning raw pointer
  only). The codebase is mid-migration to full RAII — new code must not add
  bare `new` where an owner fits.
- Disable copying with `= delete`.
- File paths: `std::filesystem`.
- Magic numbers become named constants (`DEFAULT_*`).
- Comments follow the EN tradition: `/*--- file header ---*/`, short line
  comments explaining *why*; no Doxygen essays.
- X-macro generated `case` labels align with the `default:` in the enclosing
  switch.

Architecture notes that will save you a debugging afternoon — the visitor
macros, the result-pointer VM model, the local-declaration lowering — are in
`docs/` (start with `docs/vm-architecture/`).

## Commits and Pull Requests

- Commit messages: English, [Conventional Commits](https://www.conventionalcommits.org)
  (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, ...).
- A PR should state what changed and why, and include tests for new behavior.
- Public-interface changes must update the headers under `include/nlang/` and
  the relevant `README.md` section in the same PR.
- User-visible behavior belongs in `docs/` (and `examples/` when runnable).

## Reporting Issues

Bugs and proposals go through the issue templates — for language semantics,
describe the motivation and the alternatives you considered; concrete use
cases beat abstract feature names.
