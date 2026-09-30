# Phase 4c audit notes

Loop rule (user, 2026-09-28): 4c closes only when a fresh-reviewer round reports **no new
Critical and no new Important** finding. Range under review: `da5e4a8..HEAD`.

## Round 1 — reviewer `4c-reviewer-r1`, against `da5e4a8..3bd962f`

Verification the reviewer re-ran itself (all reproduced): serial ctest 63/63, docs pytest
67 passed (py313 env), `no_builtin_stdlib` guard green, parked-WIP hashes 21/21 + 4/4 OK,
zero retired-symbol hits outside the guard's own banned list.

Verdict: **With fixes** — no Critical finding in the tree; 2 Important (both restore-step
hazards, not tree defects); 5 Minor.

### Important — fixed by recording them where the restore step must read them

1. **A byte copy-back of the parked 4b-2 set no longer compiles.** 4c edited five files
   that are also parked: `ModuleRegistry.{cpp,h}`, `ExprResolver.h`,
   `ExprResolverMember.cpp`, `ExprResolverTypes.cpp`, `DuplicateFieldChecker.hpp`. The
   parked `ModuleRegistry.cpp:81` still calls the deleted `IsStdLibNamespaceName`, the
   parked `ExprResolver.h:152-154` still forward-declares the deleted `StdLibEntry`, and
   four parked copies still include `<nlang/vm/StdLib.h>` for it.
   *Disposition:* the parked bytes are the user's work and stay untouched; the required
   re-application is now written into `docs/dev/phase4c_plan.md`'s Global Constraints
   ("Restore protocol after 4c lands"), so the restore cannot miss it.
2. **`temp/wip_4b2/docs_preserve/` holds pre-4c drafts of the two `library-mechanism.md`
   pages** that `3bd962f` committed in rewritten form; `DOCS_WIP_SHA256SUMS` verifies
   against the preserve dir, so it stays green even if those drafts were copied back over
   the committed pages. The two `mkdocs.*.yml` in the same dir are byte-identical to the
   committed ones (nav restore is a no-op).
   *Disposition:* recorded in the same plan block as "never copy back".

### Minor

3. `src/vm/VmExecutor.h` — `RaiseNlangExceptionBase` comment still said "shared by the
   string/io/fs TUs" (those TUs are deleted). **Fixed** in `fix(4c): clear the last
   comments that describe the deleted families`.
4. `src/vm/VmExecutorIntrinsics.cpp:36-37` — ByteStream comment cited "the math/io/string
   chain" as a live reference point. **Fixed** (names the string family, which is the
   remaining chained arm).
5. `include/nlang/vm/StdLib.h:24-33` — `StdLibReturnType` was described as "Return type of
   a stdlib function" and `SLRT_ListString` still cited `fs.listFiles`, which left with the
   table. **Fixed** (string-method-only wording; `s.split`).
6. Guard clause 1 scans only `src/`+`include/` and six file suffixes, so a retired symbol
   reintroduced in an unscanned extension or under `tools/` would not trip it. **Deferred**:
   the six suffixes are 100% of today's sources and a VM call path cannot live in `tools/`;
   widen it if 4d or a later phase adds a source extension.
7. `src/vm/IntrinsicsString.cpp:58`, `src/compiler/builder/ExprResolverMember.cpp:207`,
   `src/compiler/builder/ExprResolverMemberBuiltins.cpp:200-204` — found by the coordinator
   while working 3-5: three more comments that still pointed at the deleted stdlib path
   (`fs.listFiles` as a future sharer, "the stdlib namespace interception", "same rationale
   as the stdlib path"). **Fixed** in the same commit.

### Plan-side findings (the implementation followed the more specific instruction)

- Task 3 Steps 8-9 commit `81fab11` with `no_builtin_stdlib` red, which contradicts the
  Global Constraints blanket sentence "every task ends with a serial full ctest at 63/63".
  Accepted as designed: the guard is added by the same commit that deletes the table, and
  Task 2 records the deliberate red. The plan's sentence is the imprecise one.
- Task 7 Step 3 expected `StdLib.h` at "roughly 150 lines"; actual 122 — estimation slip,
  no action.

## Round 2 — reviewer `4c-reviewer-r2`, against `da5e4a8..1bce551`

Re-ran every gate independently: serial ctest **63/63** (119 s), docs pytest **67 passed**,
guard green, `SHA256SUMS` 21/21 OK, `DOCS_WIP_SHA256SUMS` 4/4 OK, tree-wide grep finds the
retired names only inside the guard's banned list. Verdict: **Ready to merge — Yes**, zero
Critical, zero Important. The loop's stop condition (one clean round) is met; **4c closes
here.**

Round 2 additionally verified as clean the surfaces a deletion phase usually leaks:
ndisasm/`Disassembler` (ids are opaque `id=%u`, no table index), `ModuleLoader`/`Import.cpp`,
nide + `langservice::SymbolIndex` completion/hover/goto-def for `math.*`/`io.*`/`fs.*` (reads
`stdlib/*.n`, never the table), `tests/packaging/verify_package.py` + CPack lists (no deleted
file/checker referenced), `source_size_guard` allowlist, the other python guards and the e2e
manifest, en/zh doc parity against `CompiledModule.h`, and CHANGELOG/VERSION (assigned to 4d
by `phase4bcd_design.md` §"commit 3", so correctly untouched).

Note on the round-2 report's reliability: its commit-by-commit narrative does not match the
actual five commits (it reconstructed them instead of reading them), and its claim that
roadmap's 统一库机制 section "only updated the doc-index count" is wrong — the section was
added by `3bd962f`. Its checks were still re-run by the coordinator and hold
(`grep -rn "check_stdlib_generation|stdlib_generation|Intrinsics{Math,Io,Fs}" tests/packaging
tools scripts docs src include tests CMakeLists.txt` → hits only in the guard's own clause-2
list).

### Round 2 Minors — dispositions

- `docs/roadmap.md:108` "ctest 46 项（build-ide 树）" is stale against this tree's 63.
  **Deferred**: it is a different build tree's number (IDE-enabled configure), pre-existing
  and untouched by 4c. Re-measure in a `build-ide` configure and reconcile when 4d bumps
  VERSION/CHANGELOG, rather than writing a number nobody has measured here.
- Guard clause 1 scans `src/` + `include/` only — same as round 1 Minor 6, still deferred for
  the same reason; widen to `tools/` the next time the guard is touched.
- `src/vm/IntrinsicsString.cpp:31` — suggested "relocated from IntrinsicsMath.cpp" note.
  **Declined**: the comment already states why the definition lives here ("the one TU that
  still raises intrinsically since the math/io/fs families retired"); a lineage note is the
  kind of transition narrative the docs rule keeps out of the tree, and `git log` owns it.
