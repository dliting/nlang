# Phase 4b-2 audit notes

Loop rule (user): 4b-2 closes only when a fresh-reviewer round reports **no new Critical and
no new Important**. Range under review: `1bce551..HEAD` (the 4b-2 feature commit `ded5bbc`
plus its follow-ups).

## Round 1 — reviewer `4b2-reviewer-r1`, against `1bce551..ded5bbc`

Verdict: **With fixes** — 0 Critical, 4 Important, 4 Minor.
Reviewer re-ran: serial ctest 63/63, `test_library_source` 15 cases, docs gate 67 passed.

Each finding was verified against the tree before disposition (receiving-code-review), not
accepted on report.

### Important

1. **`FindModuleType` bound a type without the owner filter** (`ModuleRegistry.cpp:352-366`):
   for a project module it did `pRoot->FindField(typeName)` with no `OwnerOf(*pFound) == i`
   check, unlike `CompiledInFunctions` (`ModuleRegistry.cpp:288-307`), so `mod.Foo` could bind
   a root-level `Foo` declared by another TU.
   *Verification:* confirmed, and wider than reported — the library branch also **required** a
   `namespace <path>` wrapper, so a library TU whose file carries none (members merged at the
   root, exactly the case `CompiledInFunctions` handles with its root fallback) had
   unnameable types. The owner check only becomes meaningful once the fallback exists.
   *Disposition:* **fixed** in `d7ca710`. `FindModuleType` now resolves the same container
   `CompiledInFunctions` uses (wrapper namespace when present, root otherwise) and scans
   `equal_range` for an owner-matched, bindable decl. Two load-bearing cases added:
   `TestModuleTypeWithoutNamespaceWrapper` (compiles, prints `7`) and
   `TestModuleTypeOwnerIsolation` (a foreign unit borrowing the consumer's root `Box` is
   rejected). Suite 15 → 17 cases / 18 CHECKs.
   *Method note:* non-vacuity was first proven by editing the owner condition to `true`, then
   reverting before any rebuild. That leaves a hazardous intermediate state — the CLI probe
   against the already-filtered binary reproduces the same diagnostic, use that instead.

2. **`HeadType: MemberExpr` + unguarded casts in `CollectQualifiedSegments` = UB**
   (`nlang.y:158-172`, `nlang.y:1324-1331`): `MemberExpr` includes `Expression '.' InvokeExpr`
   and arbitrary `Expression` outers, so `a.b() v;` or `a[0].x v;` — previously a syntax error —
   now reduces to `HeadType`, and the helper `static_cast`s `Outer()`/`Inner()` without a
   `Kind()` check.
   *Verification:* still present in the tree (line 168 casts unconditionally; `segs.at(0)` /
   `segs.at(1)` at 1327 also assume ≥ 2 segments).
   *Disposition:* **moves to Phase 4e**, where the fix is structural rather than a guard: a
   dedicated dotted-identifier syntactic category replaces the borrowed `MemberExpr`, so no
   arbitrary expression can reach the flattener at all. A `Kind()` guard here would be the
   surface patch 4e exists to make unnecessary.

3. **VM type tables stay bare-name-keyed; same-named library/project types collide**
   (`RegisterClass.cpp:95` `declMap[sn.Name()]`, `EmitExprNew.cpp:55` `FindClass(BaseName())`,
   `ResolveClassMetadata` via `FindClass(SuperClass()->Name())`): with `alib.Point` and a root
   `Point`, name lookup returns whichever registered first — wrong layout/ctor/super,
   silently. Newly reachable precisely because 4b-2 opened `ns.Type`.
   *Verification:* **reproduced**, after one false start. The first probe (`temp/rb2`) was
   invalid — it differed in more than the namespace, so its "collision" was not evidence. The
   second (`temp/rb3`) isolated the variable and showed a real wrong-class-layout run. The
   asymmetry is the root cause: the function table keys fully qualified names
   (`m_natives["ns.name"]`), the class/struct tables key `Name()`.
   *Disposition:* **moves to Phase 4e** (qualified VM type identity behind one seam, generic
   erased keys `"List"`/`"Dict"` unchanged). Duplicate-bare-name detection alone was declined:
   it makes the failure loud but keeps the one-symbol-space rule wrong, and 4d builds on this
   seam.

4. **Planned surface and negative paths untested**: no coverage for the unimported-namespace
   diagnostic, imported-ns-missing-type, external `.nmod` → null, a library type in a consumer
   function signature, a library type as a field of another library class or in a container,
   and deep chains `a.b.Type` — while the grammar comment advertises `pkg.mod.Type` and
   `FindModuleType`'s library branch does a single-segment `pRoot->FindField(path)`. Also
   `alib.Vec<int>` parses nowhere (`Type: NameExpr '<' TypeList '>'`), and `HeadType`'s generic
   forms cover only single-segment bases.
   *Verification:* confirmed. Single-segment resolution and the missing generic-qualified
   production are real holes; the docs sentence "resolve at the consumer side like project
   types" overstates what the VM side guarantees (see 3).
   *Disposition:* owner-isolation negative coverage landed with 1 (`d7ca710`). The rest is
   **Phase 4e scope**: dotted namespace paths get defined semantics (or an honest diagnostic),
   qualified generic type spellings get a production, and the negative matrix is written
   against that design instead of the current partial behaviour.

### Minor

- `ExprResolverMember.cpp:160-166` — the namespace branch re-assigns `m_pContext` to the value
  already set above. **Open**, folds into 4e's resolver work.
- `EmitExprCore.cpp:56-63` — body indented 8 spaces against the file's 4. **Open**, fix with
  the next touch of that file.
- `SnExpressions.h` — the comment says the qualified node "never reaches codegen", but the
  `RejectUnimportedQualifiedType`/malformed paths leave it unresolved. Harmless under the
  error-aborts-build contract. **Open**, reword when 4e rewrites the node.
- `TypeName()` asserts non-empty while `Access` treats `segs.size() < 2` as a runtime log —
  two disciplines for one invariant. **Open**, 4e (the category change removes the case).

## Assessment

Round 1's findings split cleanly: 1 was a live compiler-side binding defect, fixable and
fixed now; 2/3/4 are all faces of one missing rule — **library functions have a declared
identity, library types were given reachability without one**. Patching them one by one would
be symptom work, so 2/3/4 become **Phase 4e「type identity」** (user decision 2026-09-29,
4e before 4d). 4b-2's feature commit stays; round 2 runs against
`1bce551..HEAD` after 4e lands, since a round now would re-report 2/3/4 as open.

## Round 2 — in-session fresh pass (single-LLM mode per user instruction
2026-09-30), against `1bce551..HEAD` (= phase 5 complete, `b0ede47`)

Reviewer re-ran: serial ctest 64/64, e2e 977/6 (verbatim pre-existing six), docs 67 passed,
size guard clean — all at this HEAD during Tasks 5–8; nothing changed since.

Round 1 dispositions verified against the tree, each with evidence:

1. (was Important 1, fixed in `d7ca710`) — **still fixed**; Task 6's de-shell reduced
   `FindModuleType` further (root `equal_range` + owner filter only; the wrapper branch is
   gone with the shell syntax). `TestModuleTypeWithoutNamespaceWrapper` is now the normal
   shape; owner isolation re-verified by `moduleFunctionsProjectBranch`/`sameStem` pins.
2. (was Important 2, moved to 4e) — **fixed, guard-based rather than category-based**:
   `CollectQualifiedSegments` (grammar/nlang.y:166) now Kind-checks every link (identifier in,
   member with identifier inner, recursive outer) and returns false for call/subscript/paren
   outers — "no unchecked downcast happens here" per the in-file comment. The dedicated
   statement-head category 4e planned was measured at +2 sr on '.' and replaced by the
   `TypeName` category + guards (grammar ledger :1240-1265). No UB path remains.
3. (was Important 3, moved to 4e) — **fixed in Task 4** (`0ec7b42`): VM type/function keys are
   package-qualified behind `VmBackend::KeyOf` (17 sites across Register.cpp/RegisterClass.cpp);
   the bare-key collision Task 4's rb3 test reproduced now cannot coexist (same-name types in
   different packages conflict or coexist by qualified identity — Task 5/6 pins).
4. (was Important 4, moved to 4e) — **closed across Tasks 1/2/5/6**: dotted imports with
   matched-root packages (Task 6), deep-chain negative `a.b.Type` (Task 1), qualified
   generics on user types rejected per measured D9 behavior (Task 2 Step 7b), library types
   in signatures/fields/containers and the missing-type diagnostic (Tasks 5/6 thirdparty
   scenarios (4a)-(4c)).

Round 1 Minors re-checked at HEAD: (1) the receiver re-assign in
`ExprResolverMember.cpp` SwitchContextToReceiver — still present, still harmless (imported
stub containers keep `NK_Namespace` receivers alive, so the branch is not dead); **open**,
cosmetic. (2) `EmitExprCore.cpp` 8-space indent — file untouched this phase; **open** per the
"fix with the next touch" rule. (3) the SnExpressions.h "never reaches codegen" comment — the
node's contract is unchanged (reject paths leave it unresolved; error aborts the build);
**open** wording nit. (4) the `TypeName()` dual discipline — the category landed; the two
disciplines remain but no case reaches the divergence; **open**.

**Verdict: no new Critical, no new Important** — the 4b-2 loop closes. Remaining items are
the four carried Minors above.
