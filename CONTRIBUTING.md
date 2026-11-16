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

Full runs are batched into three tiers (`tools/gate_tier.sh`, design in
`docs/dev/gate_tiering_design.md`): T0 runs only the test families an
iteration's changed sources map to (unmapped changes fall back to the full
Debug unit suite); T1 is the phase dock — full Debug units and e2e, the IDE
tree, ASan units and the ASan memory-risk e2e subset; T2 adds full ASan e2e,
the Release build and e2e, and the docs pipeline, and runs once at version
close. Every e2e manifest entry must be selected by at least one tier
(`tools/tier_coverage_check.py`); a red leg voids the whole batch and forces
a full re-sweep.

A tier only decides which legs run; `tools/gate.sh` decides when each one may
start and what counts as its result, and the tier script starts every leg
through it. A leg is therefore judged by that script's verdict line or exit
code and never by a piped tail (a pipe reports the tail's code, which once
turned a four-failure ctest run into a recorded "all green"), and a leg that
runs tests holds a machine-wide lock while it runs, because two batches on one
machine would otherwise share the CPU the timing- and concurrency-sensitive VM
tests measure and the desktop whose foreground process owns the keyboard focus
chain the IDE tests need. The tier script never asks to run legs in parallel:
it works through its batch one leg at a time, so a tier measures one machine
at a time. Parallelism belongs to the person assembling a batch instead, and
the tool for it is the script's `--shared` flag: a command that only builds (a
configure, a compile, a docs build) still waits for any running test gate, but
overlaps other build gates, while claiming each build directory its own
command line names, so two build gates can never fight over one tree even
though different trees build side by side.

The network cases assume a working loopback. The TCP and UDP cases bind their
own peers, so they only need loopback sockets; `stdlib_net_ping_loopback` and
`stdlib_net_ping_stress` additionally need the host to answer an ICMP echo,
which a filter driver can prevent for every address, loopback included (the
raw-ICMP case only binds a socket and accepts either outcome, so it needs no
answer). The unit suite therefore does not require an answer —
`test_net_platform.cpp` pins the documented verdict set instead — so a host
with no ICMP echo still gives a green unit gate, and the strict echo proof
stays in the e2e suite, where the echo-dependent entries are environment-gated
as described below: a host that cannot answer skips them visibly instead of
failing the batch.

Tests should exercise real behavior (compile real `.n` files, execute real
bytecode), not mocks of the compiler.

Entries whose live-success premise rides on a machine capability that
ambient software can revoke — loopback ICMP echo behind VPN filter
drivers, reserved-TLD NXDOMAIN honesty behind DNS hijacks — are gated in
`tests/e2e/env_gates.txt`: an OS oracle probes the capability once per
run and the entry skips with a printed SKIP line when it is absent. A
skip is counted and visible, never a silent pass.

## Versioning and Releases

The version number lives in one place: the repository root `VERSION` file.
CMake reads it at configure time and feeds the tools' `--version`, the IDE's
About dialog, the documentation-site footer and the CPack package names.

**Manual sync is part of a feature task, not a release chore.** The user
manual carries a coverage anchor: `docs/user_manual/SYNC_BASELINE` holds the
commit whose user-visible changes the manual already reflects. Before
editing the manual, run `python tools/manual_sync_check.py` and account for
every commit it lists — each is either already reflected, not user-visible
(language surface, standard-library surface, diagnostics, exit codes and
ABI are the user-visible classes), or reflected in this edit. Commit the
manual, then run the same tool with `--update` to re-anchor. A release
requires zero drift.

**Bump the version when a change lands, not when you release:**

1. A user-visible feature addition or change bumps the minor version
   (`0.2.0 → 0.3.0`); a fix bumps the patch version (`0.2.0 → 0.2.1`).
2. With the bump, record the change under that version in
   [CHANGELOG.md](CHANGELOG.md) (Keep a Changelog style; the top section is
   the in-development version and carries no date yet). If the current
   version was never released, fold its unreleased entries into the new
   section instead of leaving an empty one. Mirror the entry in
   [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md): the root README and CHANGELOG
   are maintained as English/Chinese pairs, each file cross-linking its
   twin at the top.
3. Rebuild and run the gates — ctest includes a guard that every tool's
   `--version` echoes the file, and `verify_package.py` checks the package
   name against it.

**Releasing** means publishing the version the tree already carries:

1. Date the top changelog section and commit.
2. `git tag -a v<version> -m "NLang <version>"` and push the tag.
3. Package and upload as described in [Packaging](README.md#packaging-windows).

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
- Comments: `/*--- file header ---*/`, short line comments
  explaining *why*; no Doxygen essays.
- X-macro generated `case` labels align with the `default:` in the enclosing
  switch.

### Source size guidelines

Keep hand-written source files at or under 500 lines and functions at or
under 50 lines, with narrow registered exceptions: flat dispatch
skeletons whose cases are all a few lines, X-macro-generated bodies, and
generated code. `tools/source_size_guard/check_source_size.py` (ctest
`source_size_guard`) enforces both limits; `allowlist.json` next to it
registers every exception with a reason. Prefer shrinking the code over
growing the allowlist — remove an entry when its file is split.

Architecture notes that will save you a debugging afternoon — the visitor
macros, the result-pointer VM model, the local-declaration lowering — are in
`docs/` (start with `docs/user_manual/en/vm-architecture/`).

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
