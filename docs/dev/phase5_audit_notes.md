# Phase 5 audit notes

Loop rule (user): phase 5 closes only when a fresh-reviewer round reports **no new Critical
and no new Important**. Range under review: `d7ca710..HEAD`. Execution mode: single LLM
process (user instruction 2026-09-30), so the "fresh reviewer" rounds below are in-session
review passes with the reviewer role held apart from the implementer role; every task also
carried its own per-task diff review (records in `.superpowers/sdd/phase5_plan/progress.md`).

## Round 1 — in-session fresh pass against the whole phase (`d7ca710..b0ede47`)

Per-task reviews already verified each commit against its brief with full gates. This pass
looked for what per-task reviews cannot see: cross-task interactions, plan-level gaps, and
the known unresolved design items.

### Verified clean

- **Grammar seam**: `%expect 16` passes at every step; `sr=16/rr=0` re-measured after the
  Task 5 deletion (±0, matching the conflict-ledger families).
- **Key/write-read closure**: every table write and lookup goes through `KeyOf`/registry
  spelling (17 codegen sites; the ndisasm ctest pin exercises the read side). The negative
  control (Task 4) proved the seam is load-bearing.
- **De-shell interaction set**: same-directory bare pools (D7), cross-package duplicate types,
  library discovery vs project sources, symbol-index/registry package agreement, and the
  symbol index's package source — each was caught, fixed and pinned during Tasks 5–7; the
  pins re-ran green in every later task's full gate.
- **Public text**: `namespace` concept zero in the manual (one generic-English survivor,
  documented); predecessor-pattern scan 0 violations; diagnostics say package; CHANGELOG
  covers the phase bilingually.
- **Gates at HEAD**: build rc=0; ctest 64/64 serial; e2e 977 passed / 6 failed with the six
  failures verbatim the pre-existing set and SKIP=0; docs 67 passed; source-size guard clean;
  `git status --porcelain` at baseline noise only.

### Known open items (plan-level, reported — not new findings)

1. **Bare-type resolution still owner-blind** (R1 leftover, user decision pending): with a
   library exporting `Point` imported, the consumer's bare `Point p;` binds first-match by
   name — `Access(SnIdentifierExpr)` type candidates have no owner filter
   (`ExprResolverTypes.cpp` bare-type branch) and `ResolveClassBases`
   (`StatementResolverTypes.cpp:27-56`) takes the non-qualified branch bare. Reproduced at
   HEAD (`temp/r1gap` probe: compiles clean; the qualified form `lib.geometry.Point` is the
   only owner-true spelling). Design §R1 counted five owner-blind lookups; Tasks 3–6 covered
   three (functions, qualified types, stream literals); these two remain. Fix belongs to the
   same seam as Task 4's `KeyOf` but changes user-visible binding precedence — **user
   decision** whether phase 5 closes with it deferred (recommended: phase 6, where dotted
   imports make the ambiguity routine).
2. **Bare-name runtime surface for a user `Object`** (design §8 runtime half): the five
   compiler-side bare-name short-circuits to the builtin single-receiver are pinned as
   bare-name judgments (Task 4 comments) and were not scheduled. Same decision family as 1.
3. **`alib.Color.Green.rank()`** (method call on a package-qualified value) declines to the
   generic path — the value chain (Task 5) resolves the prefix but the method-on-qualified-
   value shape was left for the dotted-import model to revisit; currently it reports the
   generic not-found. Open UX question, not a regression.
4. **D13 + IDE human-eye acceptance** — batched for phase end per the standing deferral:
   `at main.main` / `b main.main` / `ndisasm -func main.main`, completion on `io.` and
   `vendor.graphics.`, project-properties dialog without the namespace row.

### Carried minors (no behavior impact)

- `ExprResolverMember.cpp` SwitchContextToReceiver's redundant re-assign (4b-2 Minor 1).
- `EmitExprCore.cpp:56-63` 8-space indent (4b-2 Minor 2; file untouched this phase).
- `SnExpressions.h:576` comment wording (4b-2 Minor 3).
- `RegisterUnit` duplicate diagnostic names the same path twice when one file is listed twice
  in Sources (Task 6 in-proc pin documents the shape).
- `buildGateProject` creates an empty `core/` dir unconditionally (Task 5 test scaffold).

## Verdict

**No new Critical, no new Important.** The phase-5 loop closes at `b0ede47` with the four
open items above reported for user decision / phase-6 scheduling. The 4b-2 loop closed in the
same pass (its Round 2 record lives in `docs/dev/phase4b2_audit_notes.md`).
