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
