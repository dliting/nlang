# 0.13.1a 泛型完善 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task (the user's standing directive fixes inline execution). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver 0.13.1a generic completion — static T references fully enabled plus generic free functions, via a per-frame descriptor cell and call-site hidden arguments.

**Architecture:** One delivery channel (CallFrame.instDesc field filled at the single push throat from three sources: receiver head / caller forward / call-site descIdx operand), one unified T-op read path (frame context replaces slot-0 receiver read), two language surfaces on top (cross-class static call inference + explicit `S<int>.f` spelling; generic free functions with call-site type-argument inference), then wire format v2.17 and docs.

**Tech Stack:** C++17, Flex/Bison grammar (src/compiler/grammar/nlang.y), the v0.13.1 descriptor infrastructure (GenericInstDescriptor intern pool, descriptor table with static-spelling and recipe rows).

**Spec:** docs/dev/generic_completion_design.md (committed 24e49ad3)

## Global Constraints

- Enum order is ABI: rename opcode slots in place (OP_CallFunc→OP_CallStatic), never renumber; appended enumerators go at the tail.
- Stride tables in BytecodeOps.h are the single source; disassembler/ndisasm/ndb/linker stepping derive from them — every operand change updates the group membership and nothing else.
- `NCU_FORMAT_MINOR` 16→17 exactly once (Task 1); loader rejects ≤16; all `.ncu` rebuild (no backward compat).
- v0.13.1 instance-semantics e2e (1878 baseline) stays green from Task 2 onward — every task's suite run includes the e2e family.
- Test exits ≤255; e2e probes via `D:/dev/miniconda3/envs/py313/python.exe tools/probe_run.py`; switch-case test bodies accumulate state, never `return` mid-case.
- Comments English; magic numbers as named constants (`kInstDescNone = 0xFFFF`, `kInstDescForwardCaller = 0xFFFE`).
- File line caps 500 (VmExecutorOps.cpp allowlisted; VmExecutorAlloc.cpp and DescriptorSpelling.cpp are NOT — extract rather than grow); source_size_guard must stay clean.
- Diagnostics text may change (no backward compat), but every changed diagnostic updates the err_* e2e expectation in the same task.
- Commits follow Conventional Commits; each task's cycle ends with its full-suite run green before commit.

## Review Focus

1. **gc-stress frame-slot recycling**: a generic static/free frame whose slot is filled from a recipe composed against the CALLER's slot — under `SetGcStressThresholds(4)` the caller's interned descriptor pointer must survive collections (host-memory pool, but the test must prove the fill order beats the safepoint). Pinned by `testGenericFrameSlotUnderGcStress` (Task 6).
2. **Erased-row receiver degradation**: an object whose head descriptor is null reaching a generic instance method must yield the named runtime error (`generic operation without an instance context`), not a null deref. Pinned by e2e `generic_erased_receiver.n` (Task 2).
3. **Inference vs default arguments**: an omitted trailing arg must not participate in unification (defaults fill after inference; a T-typed formal has no default — rejected). Pinned by e2e arm in `generic_free_fn_infer.n` (Task 5).
4. **Comparison-chain statements near the new expression grammar**: `a < b > c;`-shaped statements must parse exactly as before. Pinned by conflict-count baseline comparison plus `tests/e2e/generic_qualified_static.n` non-generic arms (Task 3).
5. **Cross-package stub of a generic free function, called with T-forwarding inside the consumer**: the stub's placeholder leaves must bind through the function-level bind arm and the consumer's call must compose a recipe from its own frame context. Pinned by `generic_pkg_free_fn` roundtrip (Task 7).

---

### Task 1: Delivery channel — frame descriptor cell, OP_CallStatic, intra-class static T

**Files:**
- Modify: `include/nlang/vm/CompiledModuleRecords.h` (CompiledFunction: isGeneric + typeParamNames)
- Modify: `src/vm/BytecodeOps.h` (rename OP_CallFunc/OP_CallFuncOut slots → OP_CallStatic/OP_CallStaticOut + descIdx operand; new stride group `kStrideOutMaskDescWords` for the Out variant 1+2+2+2+4; plain variant joins `kStrideThreeWords`)
- Modify: `include/nlang/vm/CallFrame.h` (add `const GenericInstDescriptor* instDesc = nullptr;`)
- Modify: `src/vm/VmExecutorFrames.cpp` (PushFrameCore fill: receiver/forward/table three-source rule)
- Modify: `src/vm/VmExecutor.h` (PushFrameCore/PushCalleeFrame/PushBoundCalleeFrame signatures gain `uint16_t descIdx, int32_t receiverHeapIdx`; ContextInstDescOf helper; NCU_FORMAT_MINOR bump lives in CompiledModule.h)
- Modify: `src/vm/VmExecutorOpsCalls.cpp` (OpCallFunc→OpCallStatic reads descIdx + fills receiver handle for instance shapes; OpCallMethodDirect/Out pass receiverHeapIdx; OpCallMethod passes thisHeapIdx; delegate arms)
- Modify: `src/vm/VmExecutorOpsDescriptor.cpp` (ReceiverTypeArgOf → read frame instDesc; ComposeArgSpelling tag-2 arm reads frame instDesc — signature gains the executing frame's descriptor pointer)
- Modify: `src/vm/IntrinsicsString.cpp` / all MakeFunc sites (reject generic targets, named diagnostic)
- Modify: `src/vm/backend/EmitExprCore.cpp` (EmitFreeFunctionCall gains descIdx param; bare intra-class generic static calls emit 0xFFFE)
- Modify: `src/compiler/DescriptorSpelling.cpp` → `src/compiler/builder/DescriptorSpelling.cpp` (DescriptorIndexFor: drop the isStatic rejection arm — generic records have a context)
- Modify: resolver static-op gates (`ExprResolverTypeGates.cpp:166-206`, `ExprResolverValues.cpp:245-283`, `ExprResolverValues.cpp:434-479`): remove the NF_Static rejection arms
- Test: `tests/e2e/generic_static_intra.n`, `tests/e2e/generic_erased_receiver.n`; update the v0.13.1 static-rejection err_* e2e expectations flipped by the gate lift
- Test: `tests/test_vm/test_generic_runtime_info.cpp` (frame-slot unit tests)

**Interfaces:**
- Produces: `CompiledFunction::isGeneric` (bool), `CompiledFunction::typeParamNames` (vector<string>), `CallFrame::instDesc`, `PushFrameCore(callee, callParamBase, outMask, outShift, boundThisIdx, descIdx, receiverHeapIdx)`, `VmExecutor::ContextInstDescOf()` (executing frame's instDesc), constants `kInstDescNone`/`kInstDescForwardCaller` in BytecodeOps.h. All later tasks consume these exact names.

- [ ] **Step 1: Write the failing e2e test**

`tests/e2e/generic_static_intra.n` — intra-class bare static call, T ops in a static body via forwarded context:

```csharp
// generic_static_intra.n — 0.13.1a: static bodies read the frame context.
import io;
class Tag<T> {
    public static string tag() {
        return typeName(T);
    }
    public static bool same(T a, T b) {
        return a == b;
    }
    public static string full(T v) {
        // the T-operation surface inside a static body: concat (both
        // orders), print, equality, as-cast. (T-argument construction
        // from static/free bodies lands in Task 6 — it needs recipe
        // rows composed from this frame context.)
        if ("v=" + v != "v=7") { return "c1"; }
        if (v + "!" != "7!") { return "c2"; }
        io.print("v=" + v);
        Object o = v;
        T back = o as T;
        if (!same(back, v)) { return "a"; }
        return tag();
    }
    public string viaInstance(int x) {
        // bare static calls forward the instance's descriptor
        if (tag() != "int") { return "bad"; }
        if (!same(x, x)) { return "bad2"; }
        if (full(x) != "int") { return "bad3"; }
        return "ok";
    }
}
int main() {
    Tag<int> t = Tag(1);
    if (t.viaInstance(1) != "ok") { return 3; }
    return 0;
}
```

Register in `tests/e2e/manifest.txt` following the v0.13.1 generic family block (line `generic_static_intra 0`).

- [ ] **Step 2: Run it to verify it fails**

Run: `D:/dev/miniconda3/envs/py313/python.exe tools/probe_run.py tests/e2e/generic_static_intra.n`
Expected: FAIL — compile error (`typeName of a type parameter requires an instance context`), exit non-zero.

- [ ] **Step 3: Record fields and wire format bump**

In CompiledModuleRecords.h, extend CompiledFunction after `isInstanceMethod`:

```cpp
//0.13.1a (v2.17): generic record — a member of a generic class row or
//a generic free function. Generic frames carry the instantiation
//descriptor cell filled at push (receiver head, caller forward, or
//call-site descriptor row); T operations read it. Non-generic records
//leave the cell null and pay nothing.
bool isGeneric = false;
//v2.17: declared type-parameter names (generic records only). The
//stub rebuild matches these to bind placeholder leaves in signatures,
//mirroring CompiledClass::typeParamNames.
std::vector<std::string> typeParamNames;
```

Bump `NCU_FORMAT_MINOR` in CompiledModule.h 16→17 and make the loader reject older (`< 17`) with the existing version-mismatch diagnostic shape.

- [ ] **Step 4: CallFrame field and push fill**

CallFrame.h after `outShift`:

```cpp
//0.13.1a: the frame's instantiation context — the descriptor every
//T operation in this body reads. Filled at push for generic records:
//instance shapes from the receiver's head descriptor, receiver-less
//direct calls from the call-site descriptor row (or the caller's own
//cell via kInstDescForwardCaller). Null = no context (non-generic
//records; erased-row degradation reports the named runtime error).
const GenericInstDescriptor* instDesc = nullptr;
```

PushFrameCore fill (after `fr.outShift = outShift;`, before CheckGCSafepoint):

```cpp
//0.13.1a: descriptor fill — one rule, three sources, generic records
//only. Receiver head wins (instance/bound shapes); kInstDescForwardCaller
//inherits the caller's own cell (dictionary forwarding); a table index
//composes at push against the caller's frame (recipe rows read the
//caller context). Order matters: the GC safepoint below must not run
//between source read and store — the interned pool is host memory and
//never collected, so the pointer is stable regardless.
fr.instDesc = nullptr;
if (callee.isGeneric) {
    if (receiverHeapIdx > 0) {
        fr.instDesc = InstDescriptorOf(receiverHeapIdx);
    } else if (descIdx == kInstDescForwardCaller) {
        fr.instDesc = callerInstDesc;
    } else if (descIdx != kInstDescNone) {
        fr.instDesc = CastTargetDescriptor(descIdx, callerLocals);
    }
}
```

Capture `const GenericInstDescriptor* callerInstDesc = EvalStack().back().instDesc;` alongside the existing caller captures at the top of PushFrameCore. Signatures (VmExecutor.h declarations + all definitions):

```cpp
uint8_t* PushFrameCore(const CompiledFunction& callee,
    uint16_t callParamBase, uint32_t outMask, uint8_t outShift,
    int32_t boundThisIdx, uint16_t descIdx, int32_t receiverHeapIdx);
uint8_t* PushCalleeFrame(const CompiledFunction& callee,
    uint16_t callParamBase, uint32_t outMask, uint8_t outShift,
    uint16_t descIdx, int32_t receiverHeapIdx);
uint8_t* PushBoundCalleeFrame(const CompiledFunction& callee,
    int32_t thisIdx, uint16_t callParamBase, uint32_t outMask);
```

PushBoundCalleeFrame computes `descIdx = kInstDescNone, receiverHeapIdx = thisIdx` internally. Add the executor helper (VmExecutor.h inline or VmExecutorOpsDescriptor.cpp):

```cpp
//The executing frame's instantiation context — every T operation and
//recipe composition reads this (0.13.1a unified the v0.13.1 slot-0
//receiver read onto the frame cell; instance frames carry the same
//descriptor their receiver's head does).
const GenericInstDescriptor* ContextInstDescOf() const {
    return EvalStack().back().instDesc;
}
```

- [ ] **Step 5: Opcode rename and operand**

BytecodeOps.h: rename enum slots in place (`OP_CallFunc` → `OP_CallStatic`, `OP_CallFuncOut` → `OP_CallStaticOut` — positions unchanged, comments updated: `uint16 funcIdx, uint16 callParamBase, uint16 descIdx`). Define at the top of the file:

```cpp
//0.13.1a call-site descriptor operand values (OP_CallStatic family).
static constexpr uint16_t kInstDescNone = 0xFFFF;          //no descriptor
static constexpr uint16_t kInstDescForwardCaller = 0xFFFE; //inherit caller frame cell
```

Stride groups: move `OP_CallStatic` from `kStrideTwoWords` to `kStrideThreeWords`; move `OP_CallStaticOut` from `kStrideOutMaskWords` into a new group `kStrideOutMaskDescWords` with stride `1 + 2 + 2 + 2 + 4`; `OP_CallMethodDirect`/`OP_CallMethodDirectOut` stay in their groups unchanged. Grep the four old names across src/ and tests/ and update mechanically (emitters, executors, disassembler comments, ndisasm snapshots, linker if it names them).

- [ ] **Step 6: Executor arms**

OpCallStatic (the renamed OpCallFunc): read `descIdx` after callParamBase; for `callee.isInstanceMethod && callee.isGeneric`, read the receiver handle from `locals + callParamBase` (memcpy int32, as OpCallMethod does) and pass it as receiverHeapIdx; pass `descIdx`, receiverHeapIdx to PushCalleeFrame. OpCallMethodDirect/Out: same receiver read when `callee.isGeneric && callee.isInstanceMethod`, `descIdx = kInstDescNone`. OpCallMethod: pass `receiverHeapIdx = thisHeapIdx` (already read for the class dispatch). CallDelegateBound: PushBoundCalleeFrame unchanged signature (receiver source inside). CallDelegateFree: guard `callee.isGeneric` → named runtime error (defense in depth; MakeFunc already rejects).

- [ ] **Step 7: T-op read switch and gate lift**

VmExecutorOpsDescriptor.cpp: `ReceiverTypeArgOf` body becomes `const GenericInstDescriptor* desc = ContextInstDescOf(); if (!desc || ordinal >= desc->TypeArgs().size()) throw ...("generic operation without an instance context");` (rename to `ContextTypeArgOf` — update the five call sites: OpTypeNameArg, OpTypeParamEquality, OpTypeArgToStr, OpAsTypeParam, and the OP_New recipe path). `ComposeArgSpelling` tag-2 arm: replace the slot-0 memcpy + InstDescriptorOf read with the composing frame's `ContextInstDescOf()` — thread the descriptor pointer into `ComposeInstDescriptor(recipe, locals, instDesc)` from the OP_New/OP_CallStatic call sites. Resolver gates: delete the three NF_Static arms (ExprResolverTypeGates.cpp RejectTypeParamOperand, ExprResolverValues.cpp ResolveAsCastTarget + Access(SnTypeNameExpr) static arm) and DescriptorSpelling.cpp's isStatic arm in DescriptorIndexFor — generic records carry a context; non-generic records cannot bind T at all, so nothing else can reach these paths. Grep for the four diagnostic texts to find every flipped expectation.

- [ ] **Step 8: MakeFunc rejection**

At each MakeFunc/MakeBoundFunc/MakeVFunc emit-or-exec site (VmExecutorOpsCalls.cpp:50-143 family): when the target record `isGeneric`, log the named diagnostic on the compiler side instead — the resolver must reject `MakeFunc`-shaped references to generic functions at resolve time: `"a generic function cannot be referenced as a value; call it directly"` (add the resolver arm where function values bind names to records; executor-side guard stays as defense).

- [ ] **Step 9: Emission — bare intra-class generic static calls forward**

EmitExprCore.cpp `EmitFreeFunctionCall` gains `uint16_t descIdx` (signature `EmitFreeFunctionCall(int funcIndex, uint16_t descIdx, ...)`), emitting `EmitUint16(descIdx)` after callParamBase (both variants). The static bare-call path (`Access(SnInvokeExpr)` → EmitInvokeDispatch → EmitFreeFunctionCall): when `callee->isGeneric()` (new SnFunction accessor set at registration — Task 1 needs the resolver to set isGeneric on the CompiledFunction record for generic-class statics AND later free functions; for now Register.cpp sets it when the owning class `IsGenericDeclaration()`), emit `kInstDescForwardCaller`. Non-generic: `kInstDescNone`. EmitMemberStaticCall routes stay on OP_CallMethodDirect (no operand) until Task 2.

- [ ] **Step 10: Run the e2e test to verify it passes**

Run: `D:/dev/miniconda3/envs/py313/python.exe tools/probe_run.py tests/e2e/generic_static_intra.n`
Expected: PASS, exit 0.

- [ ] **Step 11: Flipped-expectation sweep, negative probes, ndb stepping, unit test**

The lifted static-context diagnostics flip these manifest pins (update each file + manifest line in the same commit): `err_generic_t_eq_static`, `err_as_type_param_static`, `err_generic_static_ctor`, and any typeName static arm (`grep -n "requires an instance context" tests/e2e/manifest.txt tests/e2e/*.n` enumerates them all). The static-body arms become positive arms — move them into `generic_static_intra.n` or sibling positive files. These pins STAY (they reject T operands regardless of context — different gates, untouched): `err_generic_t_order` (ordering operators on T), `err_generic_t_arith` (arithmetic on T), `err_static_generic_field` (static fields of generic classes — the compensation pattern remains the answer).

Add two negative probes to the manifest:

```
err_generic_func_value compile_error referenced as a value
err_static_t_order_stays compile_error cannot be applied to a type parameter
```

The MakeFunc probe file references a generic function as a value (the existing value surface is `func<int> f = b.get;` — the probe does the same against `Tag<int>.same`-shaped statics and later `identity`-shaped free functions once Task 4 lands):

```csharp
// err_generic_func_value.n — a generic function cannot be referenced
// as a value: a value has no descriptor to pair with.
class Tag<T> {
    public static bool same(T a, T b) { return a == b; }
}
int main() {
    func<bool> f = Tag<int>.same;
    return f(1, 1) ? 0 : 3;
}
```

ndb stepping verification: the debugger tests step by stride-table-derived layouts (`tests/test_vm/test_debugger*.cpp` family, `tests/check_ndb_native.py`). Rebuild and run the debugger test family + `D:/dev/miniconda3/envs/py313/python.exe tests/check_ndb_native.py` — a stale stride breaks single-stepping across `OP_CallStatic`. Expected: all green.

Add the frame-slot unit test to test_generic_runtime_info.cpp:

```cpp
//0.13.1a: a generic static frame carries the forwarded caller context;
//the cell is null for non-generic records.
TEST_CASE_METHOD(GenericRuntimeInfoFixture, "genericStaticFrameCarriesForwardedContext") {
    // compile a program whose instance method calls the class's static
    // method (bare), read typeName(T) inside — asserts via the program's
    // own exit code (same harness as recycledSlotsCarryNoDescriptor)
}
```

Run the full unit suite: `ctest --test-dir build -R test_generic_runtime_info --output-on-failure` → PASS.

- [ ] **Step 12: Full suite + commit**

Run: Debug ctest family + e2e suite (the established per-task sweep). Expected: all green including the 1878 e2e baseline (new file added, no case count change beyond +1). Commit:

```bash
git add -A -- src include tests docs/dev/generic_completion_design.md
git commit -m "feat(vm): 0.13.1a frame descriptor cell and OP_CallStatic — static T via forwarded context"
```

### Task 2: Cross-class static calls — inference arm

**Files:**
- Modify: `src/compiler/builder/ExprResolverGenerics.cpp` (generalize WrapGenericElemArgs' context gate; new inference entry)
- Create: `include/nlang/compiler/TypeArgInference.h` + `src/compiler/builder/TypeArgInference.cpp` (unification walker)
- Modify: `src/vm/backend/EmitExprMemberStatic.cpp` (EmitMemberStaticCall: generic callee → EmitMemberDirectMethodCall must carry descIdx — switch statics to the OP_CallStatic routing via EmitFreeFunctionCall-shaped helper)
- Modify: `src/vm/backend/Slots.cpp` (no change expected; verify FunctionSlotFor resolves the template row's statics)
- Test: `tests/e2e/generic_static_infer.n`, `tests/e2e/generic_erased_receiver.n` (extends `deep_generic_param_desc_degrade.n`'s established erased-row setup)

**Interfaces:**
- Consumes: Task 1's record flags/descIdx plumbing; CanonicalArgSpelling (TypeArgSpelling.cpp) for actual-type spellings; PlaceholderFormalsOf (ExprResolverGenerics.cpp:209-225).
- Produces: `bool InferTypeArgsForCall(SnFunction& tmplMember, const std::vector<SnExpression*>& actuals, std::vector<TypeArgDesc>& outArgs)` in TypeArgInference.h — unifies each actual's `EvalDataType()` against the formal type trees, filling one TypeArgDesc per type parameter; returns false on conflict/no-binding.

- [ ] **Step 1: Write the failing e2e test**

`tests/e2e/generic_static_infer.n`:

```csharp
// generic_static_infer.n — 0.13.1a: cross-class static calls infer T.
class Maker<T> {
    public static T pass(T v) {
        return v;
    }
}
class Second<T> {
    public string use(int x) {
        // generic caller forwarding into another class's static:
        // recipe row composed from this frame's context
        if (Maker.pass(x) != x) { return "a"; }
        return "ok";
    }
}
int main() {
    Second<int> s = Second(1);
    if (s.use(5) != "ok") { return 3; }
    if (Maker.pass(7) != 7) { return 4; }        // non-generic caller, T from args
    string r = Maker.pass("t");
    if (r != "t") { return 5; }                  // inference per call site
    return 0;
}
```

DESIGN NOTE for the executor: a zero-argument static (`Maker.kind()`-shaped) has NO T-carrying argument — inference has nothing to bind from. The spec's D5 says inference failure names the parameter and suggests the qualified spelling; the zero-arg surface arrives with Task 3's explicit spelling (`Pair<int, string>.sig()` covers it). Task 3 registers the negative probe.

- [ ] **Step 1b: Erased-row degradation arm**

Extend `tests/e2e/deep_generic_param_desc_degrade.n` (reuse its established erased-row receiver setup verbatim): add an arm where an erased-row object reaches a generic instance method whose body makes a BARE static call (`tag()`-shaped, 0xFFFE forward) — the forward copies the null cell into the static frame, and the static body's T op must raise the named runtime error (`generic operation without an instance context`), catchable or exit-coded per the existing test's protocol. This is Review Focus 2's pin: null slot → named error, never a null deref. Manifest line stays green (extend the expected accumulation, same exit-code protocol).

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL — `Maker.pass(x)` reports `Incompatible type "int".` (the three-link chain: no wrap context).

- [ ] **Step 3: Inference walker**

TypeArgInference.cpp — unify a formal type tree against a concrete resolved type. The formal side is the template row member's registered placeholder leaves (PlaceholderFormalsOf shape); the actual side is `EvalDataType()` results. Mirror CanonicalArgSpelling's DataType walk (TypeArgSpelling.cpp) — its arms enumerate the resolved-type shapes this walker must match:

```cpp
//Unify one formal tree node against one actual resolved type.
//formalLeaf is a registered placeholder → bind ordinal → canonical
//spelling of the actual (kinds payload from the same walk the
//TypeArgDesc constructor uses). Arrays: rank must match, recurse into
//the element. Instantiations: erased row name must match, recurse
//per-argument. Concrete leaves: canonical spellings must be equal.
//A parameter bound twice with different spellings → false.
```

Exact-walk note: reuse `CanonicalArgSpelling(const DataType&)` for leaf bindings so the inferred spelling is byte-identical to what DescriptorIndexFor would intern — the descIdx row then composes from the same spelling table.

- [ ] **Step 4: Resolver hook**

ExprResolverGenerics.cpp: WrapGenericElemArgs' early-return gate (`m_pContext->Kind() != NK_ClassDecl`) gains the alternate context: when the invoke's bound callee is a member of a generic template row (`generics.IsPlaceholder` on its formals) and the calling context is NOT an instantiation, run InferTypeArgsForCall against the actuals; on success, build the substituted formal trees (SynthTypeExprFromDesc over the inferred TypeArgDesc list) and run the existing per-arg fixup on them; on failure, the named diagnostic: `"cannot infer type argument for T in <CalleeName>(...); spell the instantiation: <Template><<T>>(...)"`. Emit-side: EmitMemberStaticCall for a generic callee switches from EmitMemberDirectMethodCall to the OP_CallStatic helper with the descIdx that DescriptorIndexFor produced (store it on the invoke node the same way OP_New stores descIdx — follow the existing descIdx-on-node pattern from v0.13.1 Task 4).

- [ ] **Step 5: Run both e2e files**

`generic_static_infer.n` PASS; `generic_static_intra.n` still PASS; v0.13.1 generic family e2e green.

- [ ] **Step 6: Full suite + commit**

```bash
git add -A -- src include tests
git commit -m "feat(compiler): cross-class generic static calls — call-site type-argument inference"
```

### Task 3: Explicit qualified spelling `S<int>.f`

**Files:**
- Modify: `src/compiler/grammar/nlang.y` (expression-position generic spelling production for type-name receivers; OT_TypeLt scan-flag family)
- Modify: resolver type-receiver path (`ExprResolverInvokeReceiver.cpp` ReceiverNamesType + the generic instance resolve arm TryResolveDeclaredGenericInstance alignment)
- Test: `tests/e2e/generic_qualified_static.n`; conflict-count baseline comparison

**Interfaces:**
- Consumes: Task 2's inference entry (explicit args REPLACE inference — the spelled args mint like Type-position today).
- Produces: expression-position `Identifier '<' TypeList '>' '.' member` reaching the static-call emit with a concrete descIdx.

- [ ] **Step 1: Capture the conflict baseline**

Run the bison build as-is and record the conflict counts from the grammar build log (`bison` reports them; the repo pins them — check the nlang.y header comment for the current numbers 31 s/r + 5 r/r). The Task's grammar work must land at the SAME counts or add only the accepted epsilon family (the FormalParamList precedent documents the family).

- [ ] **Step 2: Write the failing e2e test**

`tests/e2e/generic_qualified_static.n`:

```csharp
// generic_qualified_static.n — 0.13.1a: explicit instantiation spelling.
class Pair<K, V> {
    public static string sig() {
        return typeName(K) + "," + typeName(V);
    }
    public static K firstK(K k, V v) {
        return k;
    }
}
class Hold<U> {
    public U held;
    public Hold(U v) { held = v; }
}
int main() {
    if (Pair<int, string>.sig() != "int,string") { return 3; }
    if (Pair<int, string>.firstK(4, "x") != 4) { return 4; }
    // the constructor-call sibling of the same production: a concrete
    // spelling in invoke position (OP_New rides descIdx since v0.13.1)
    Hold<int> h = Hold<int>(5);
    if (h.held != 5) { return 5; }
    return 0;
}
```

Plus a negative probe registered in the negative-cases list: `Maker.kind()` (zero-arg, no spelling) must fail with the Task 2 diagnostic.

- [ ] **Step 3: Grammar arms**

nlang.y: the MemberExpr chain gains the generic-qualified receiver production. Model it on the HeadType arms' dual spelling (scan-flagged OT_TypeLt vs plain '<' with IncGenericDepth mid-rule, lines 2052-2061): a `QualifiedGenericReceiver: TT_Identifier '<' TypeList '>'` nonterminal feeding BOTH downstream shapes — the qualified static member call (`... '.' member`) and the generic constructor call (`... '(' ArgumentList ')'`, the `Hold<int>(5)` arm above). Entry arms on the statement/expression position's scan flag reading — exactly the mechanism HeadType uses at statement head. Verify: comparison statements (`a < b > c;` shapes over variables) still parse as comparisons — the scanner flag arms on the same conditions HeadType already relies on; add parse-level unit coverage for the ambiguity shapes: `a < b > c;` (comparison chain), `f(a < b, c > d)` (comparison inside call args), `Hold<int>(5)` (spelling inside call args), `x = a < b;` (comparison in assignment).

- [ ] **Step 4: Resolve + emit**

The generic receiver resolves through the Type-position arm's logic (arity check vs TypeParamNames, RejectInvalidTypeArg, RejectMultiSlotTypeArgs) producing an instantiation-typed receiver; the member invoke dispatches through the Task 2 static path with the spelled args driving DescriptorIndexFor directly (no inference). typeName(T) inside `sig()` reads the frame context filled from the descIdx row.

- [ ] **Step 5: Run + conflict counts + full suite + commit**

Conflict counts equal to baseline (or the documented accepted family); e2e PASS; suite green.

```bash
git add -A -- src include tests
git commit -m "feat(compiler): expression-position generic spelling for static member calls"
```

### Task 4: Generic free functions — declaration, binding, basic calls

**Files:**
- Modify: `src/compiler/grammar/nlang.y` (TopBareHeader/TopModHeader families gain `'<' IdentifierList '>'` after the name)
- Modify: `src/compiler/SnMisc.cpp` or the SnFunction home file (TypeParamNames/IsGenericDeclaration accessors on SnFunction — mirror SnClassDecl's shape)
- Modify: `src/compiler/builder/ExprResolverGenericMint.cpp` (BindTypeParamPlaceholderIfAny: NK_FunctionDecl arm)
- Modify: `src/compiler/Register.cpp` (RegisterFunctions: set isGeneric + typeParamNames on the record)
- Modify: `src/compiler/builder/TypeArgInference.cpp` (free-function entry: formals from the function's own placeholder formals)
- Modify: `src/vm/backend/EmitExprCore.cpp` (free-function invoke emission: descIdx from inference)
- Test: `tests/e2e/generic_free_fn.n`

**Interfaces:**
- Consumes: Task 1 record fields; Task 2's InferTypeArgsForCall (free-function formals feed the same walker).
- Produces: `SnFunction::TypeParamNames()` / `SnFunction::IsGenericDeclaration()`; free-function CompiledFunction records with isGeneric+typeParamNames set.

- [ ] **Step 1: Write the failing e2e test**

`tests/e2e/generic_free_fn.n`:

```csharp
// generic_free_fn.n — 0.13.1a: generic free functions.
T identity<T>(T v) {
    return v;
}
string idTag<T>(T v) {
    return typeName(T);
}
int main() {
    if (identity(3) != 3) { return 3; }
    if (identity("hi") != "hi") { return 4; }
    if (idTag(2.5) != "double") { return 5; }
    if (idTag("s") != "string") { return 6; }
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: FAIL — syntax error at `identity<T>` (no grammar arm).

- [ ] **Step 3: Grammar + node**

nlang.y TopBareHeader/TopModHeader arms: after `TT_Identifier`, `'<' TT_Identifier List '>'` before `'('` — declaration position, no expression ambiguity (established ruling). SnFunction gains `m_typeParamNames` + `TypeParamNames()` + `IsGenericDeclaration()` (mirror SnClassDecl's accessors; the parser stores the identifier list). Conflict counts unchanged (new arms derive on distinct lookaheads).

- [ ] **Step 4: Binding arm**

BindTypeParamPlaceholderIfAny: the parent walk gains the NK_FunctionDecl arm BEFORE the NK_ClassDecl arm's loop exit (a free function has no class parent; the walk must not skip the function level):

```cpp
if (kind == NK_FunctionDecl)
{
    auto& fn = static_cast<SnFunctionDecl&>(*p);
    if (!fn.IsGenericDeclaration())
        return TypeParamBindResult::NotApplicable;
    //mirror the class arm: PlaceholderFor keyed on the function node,
    //ResolveFieldExprAs onto the placeholder
}
```

(Exact node kind name follows whatever the grammar produces for top-level functions — SnFunction vs an SnFunctionDecl wrapper; anchor on the existing node the resolver receives for free function bodies.)

- [ ] **Step 5: Registration + inference + emission**

Register.cpp RegisterFunctions: `cf.isGeneric = func->IsGenericDeclaration(); cf.typeParamNames = func->TypeParamNames();`. The free-function invoke path (the resolver binds `max(3, 7)` through FixupParamTypesWithBindings at ExprResolverInvoke.cpp:395): when the bound callee IsGenericDeclaration, run InferTypeArgsForCall with the function's own formals; fixup args against substituted formals; store descIdx on the invoke; EmitExprCore free-function emission reads it (kInstDescNone when non-generic).

- [ ] **Step 6: Run + full suite + commit**

`generic_free_fn.n` PASS; suite green.

```bash
git add -A -- src include tests
git commit -m "feat(compiler): generic free functions — declaration, T binding, call-site inference"
```

### Task 5: Inference shapes and diagnostics

**Files:**
- Modify: `src/compiler/builder/TypeArgInference.cpp` (array/nested/return-position arms)
- Modify: `src/compiler/builder/ExprResolverGenerics.cpp` (mismatch/no-binding diagnostics)
- Test: `tests/e2e/generic_free_fn_infer.n`; negative probes

**Interfaces:**
- Consumes/Produces: same walker; this task completes its arm set.

- [ ] **Step 1: Write the failing e2e test**

`tests/e2e/generic_free_fn_infer.n`:

```csharp
// generic_free_fn_infer.n — 0.13.1a: inference shapes.
// (List/Dict live in the builtin containers module, reachable bare.)
T firstOf<T>(T[] xs) {
    return xs[0];
}
T unwrap<T>(List<T> xs) {
    return xs.get(0);
}
List<T> makeList<T>() {
    return {};          // target-typed initializer fills the body's return
}
string kindOf<T>(T v) {
    return typeName(T);
}
T max<T>(T a, T b) {
    if (a < b) { return b; }
    return a;
}
int main() {
    int[] a = [1, 2, 3];
    if (firstOf(a) != 1) { return 3; }              // T[] shape, rank-consistent
    List<string> ls = {};
    ls.add("x");
    if (unwrap(ls) != "x") { return 4; }            // nested instantiation
    if (max(3, 7) != 7) { return 5; }
    if (max(3.0, 7.0) != 7.0) { return 6; }         // exact match, no promotion
    // return-position-only T: inferred from the assignment target
    List<string> made = makeList();
    made.add("y");
    if (made.get(0) != "y") { return 7; }
    return 0;
}
```

DESIGN NOTE (binding on the executor): the spec's return-position-only inference ("从赋值目标推断") applies when T appears ONLY in the return type (makeList above): the expected type threads from the assignment/declaration LHS. Implement that arm by consulting the enclosing assignment/declaration's resolved target type at the invoke site (the resolve path of variable declarations resolves the declared type before the initializer — hook where the initializer invoke can see it). A T-only-return function with NO assignment target reports the no-binding diagnostic. Negative probes (negative-cases list in the manifest, each `compile_error` + substring): `max(3, 7.0)`-shaped mismatch → `"conflicting type arguments for T: int vs double"`-shaped named diagnostic (exact match, no implicit numeric promotion); bare `makeList();` in a discarded-expression statement → the no-binding diagnostic; a declared `T defVal<T>(T v = expr)` → the T-formal default-value rejection diagnostic. NOTE: construction of generic instances from T inside static/free bodies (`Hold<T>` recipe arms) is Task 6 scope — this file deliberately avoids it.

- [ ] **Step 2: Implement arms + diagnostics**

Array arm: rank equality then element recurse. Nested instantiation arm: erased row name equality then per-argument recurse (struct args follow 0.13.0 legality; func args rejected per RejectMultiSlotTypeArgs — the walker inherits the same rejection on inferred spellings). Return-position arm: bind from the expected type when available; diagnostic otherwise: `"cannot infer type argument for T: T appears only in the return type; assign the result to a typed target"`.

- [ ] **Step 3: Default-arg interaction arm**

A T-typed formal must reject a default value at declaration (`T def<T>(T v = ...)`) — named diagnostic mirroring the class-template T-initializer rule. Omitted trailing args never enter unification (defaults fill after the signature check).

- [ ] **Step 4: Run + suite + commit**

```bash
git add -A -- src include tests
git commit -m "feat(compiler): complete inference arms — arrays, nested instantiations, return position"
```

### Task 6: Generic-calling-generic — recipes and forwarding between contexts

**Files:**
- Modify: `src/compiler/builder/DescriptorSpelling.cpp` (DescriptorIndexFor: recipe emission gains the context source — the existing tag-2 ARG_THIS arm IS the context source after Task 1's re-source; emit recipes when inferred args contain placeholders)
- Modify: `src/vm/backend/EmitExprCore.cpp` + `EmitExprMemberStatic.cpp` + `EmitExprNew.cpp`/`EmitCtorCall.cpp` (generic callers emit descIdx pointing at recipe rows; same-template forwarding stays 0xFFFE; constructor calls in generic bodies get the inference/recipe entry)
- Test: `tests/e2e/generic_fn_calls_fn.n`; unit `testGenericFrameSlotUnderGcStress`

**Interfaces:**
- Consumes: recipe encoding (CompiledModule.h DescriptorEntry tags 0-4), Task 1's compose-at-bridge.
- Produces: complete inter-generic call matrix.

- [ ] **Step 1: Write the failing e2e test**

`tests/e2e/generic_fn_calls_fn.n`:

```csharp
// generic_fn_calls_fn.n — 0.13.1a: inter-generic call matrix.
class Hold<U> {
    public U held;
    public Hold(U v) { held = v; }
}
class Ctor<T> {
    public static string probe(T v) {
        // generic-class static caller → free function carrying the
        // caller's T: the callee descIdx is a RECIPE row composed
        // from this frame's context at the push bridge
        return freeTag(v);
    }
    public static Hold<T> wrap(T v) {
        // T-argument construction inside a static body, bare form:
        // the constructor's U unifies against the caller's T and the
        // OP_New recipe composes Hold<T> from the frame context
        Hold<T> h = Hold(v);
        return h;
    }
    public static Hold<T> wrapSpelled(T v) {
        // the same construction through Task 3's spelled form
        return Hold<T>(v);
    }
}
T freeTag<T>(T v) {
    return typeName(T);
}
T freeEcho<T>(T v) {
    return v;
}
// cross-template forwarding: the callee is a DIFFERENT template, so the
// caller's T flows through a RECIPE row (a blind 0xFFFE forward would be
// ordinal-unsound across templates)
T freeFwd<T>(T v) {
    return freeEcho(v);
}
// recursion: a generic function calling itself with its own T
T rfn<T>(T v, int n) {
    if (n <= 0) { return v; }
    return rfn(v, n - 1);
}
int main() {
    // free → free, caller T flows into the callee's T: inference
    // unifies freeEcho's U against the caller placeholder and the
    // descriptors differ per template, so this rides a recipe row
    if (freeFwd(8) != 8) { return 3; }
    // nested inference: the inner call infers first, the outer composes
    if (freeTag(freeEcho(9)) != "int") { return 4; }
    // recursion with T forwarding (0xFFFE — same template, same instance)
    if (rfn(6, 3) != 6) { return 5; }
    // generic-class static → free function with the caller's T (recipe)
    if (Ctor<int>.probe(5) != "int") { return 6; }
    if (Ctor<string>.probe("s") != "string") { return 7; }
    // T-argument construction from a static body: bare + spelled forms
    if (Ctor<int>.wrap(4).held != 4) { return 8; }
    if (Ctor<string>.wrapSpelled("w").held != "w") { return 9; }
    return 0;
}
```

FORWARDING SOUNDNESS RULING (binding on the executor): `kInstDescForwardCaller` (0xFFFE) is emitted ONLY when the callee's template row equals the caller's own template row — recursive self-calls and same-template static-to-static bare calls (Task 1's intra-class family). A cross-template callee whose inferred argument mentions the caller's T gets a RECIPE row, never a blind forward: forwarding a foreign descriptor is ordinal-unsound in general (`Pair<K, V>` statics have two ordinals; a single-T caller forwarding its cell would misalign them). The emission-side check compares the callee's owning template declaration against the caller's enclosing template declaration.

The construction arms (`wrap`/`wrapSpelled`) satisfy the spec pin "泛型静态/游离体内以T实参构造（Hold<T>配方臂）": the constructor's formal placeholder unifies against the caller's T, and the OP_New recipe's tag-2 context sources compose at the bridge (Task 1's re-source makes the recipe read the frame cell instead of slot 0). If Task 2's inference entry needs a constructor-receiver sibling (the formal source is the template row's constructor formals instead of a member's), extend `InferTypeArgsForCall` with that entry — same walker, different formal provider.

- [ ] **Step 2: Recipe emission for inferred-placeholder args**

DescriptorIndexFor: when an inferred arg spelling contains a bound placeholder (the caller's own type parameter), emit a recipe row whose tag-2 sources name the caller ordinals — the EXISTING recipe machinery (TEMPLATE_HEAD + AppendArgRecipe with ARG_THIS entries) already encodes this; the Task's work is the resolver-side decision "inferred arg references my own T → recipe row, descIdx = recipe entry" instead of a static-spelling row. 0xFFFE stays reserved for the same-template family per the soundness ruling above (recursion, same-template bare statics) — never as a shortcut for cross-template forwarding.

- [ ] **Step 3: gc-stress unit test**

test_generic_runtime_info.cpp `testGenericFrameSlotUnderGcStress`: the Task 1 fixture shape with `SetGcStressThresholds(4)` — a generic static chain (static→free with recipe) under per-allocation collections; assert the program's typeName verdicts (exit-code protocol) AND post-run `m_instDescByHeap` invariants stay kind-restricted (the Review Focus 1 pin).

- [ ] **Step 4: Run + suite + commit**

```bash
git add -A -- src include tests
git commit -m "feat(vm): inter-generic calls — context recipe rows and caller forwarding"
```

### Task 7: Wire format — function template records and cross-package roundtrip

**Files:**
- Modify: `src/vm/ModuleSaver.cpp` (function record: isGeneric flag byte v2.17 + typeParamNames after instanceMethodFlag; T-typed paramTypeDescs already serialize RTK_TypeParam leaves from v0.13.1 — verify free functions hit the same path)
- Modify: `src/vm/ModuleLoader*.cpp` (load the new fields; stub rebuild: generic free function stubs carry typeParamNames + placeholder leaves — mirror the CompiledClass stub rebuild at ModuleLoaderRecordsClasses.cpp:227)
- Modify: `src/compiler/builder/ModuleBuilderImports.cpp` (import admission for generic free functions)
- Test: `tests/e2e/generic_pkg_free_fn.n` (sourceless-package roundtrip), cross-unit merge extension of the v0.13.1 proj_generic_cross_unit pattern

**Interfaces:**
- Consumes: v0.13.1 descriptor-table merge (per-unit, spelling+recipe key).
- Produces: sourceless packages exporting generic free functions, callable from consumers.

- [ ] **Step 1: Write the failing roundtrip test**

`tests/e2e/generic_pkg_free_fn.n` + a prebuilt package fixture: producer `.n` with `T pkgPick<T>(T a, T b) { return a; }` compiled to `.npkg`; consumer imports and calls `pkgPick(3, 4)` and, inside a consumer generic function, `pkgPick(v, v)` (T-forwarding through the stub — Review Focus 5 pin). Assert verdicts via exit codes.

- [ ] **Step 2: Wire + stub rebuild + merge**

Serialize/load the two new fields (flag-byte order: native, static, instanceMethod, generic — appending v2.17 bytes in the established order); stub rebuild produces a generic SnFunction declaration (typeParamNames set, signature leaves synthesized RTK_TypeParam via SynthTypeExprFromDesc, bound by the Task 4 function-level arm); descriptor-table merge already keys (spelling, recipe) — verify cross-unit recipe rows merge (extend the v0.13.1 linker test).

- [ ] **Step 3: Run + suite + commit**

```bash
git add -A -- src include tests
git commit -m "feat(vm): v2.17 wire — generic function records, stub rebuild, cross-package calls"
```

### Task 8: Version pipeline and docs

**Files:**
- Modify: `CMakeLists.txt` (VERSION regex tolerates the letter tag; project() takes the numeric prefix; CPACK_PACKAGE_VERSION the full string)
- Modify: `VERSION` → `0.13.1a`
- Modify: `docs/` (语言规格泛型章/静态成员章/运算符页; user manual generics + compensation-pattern rewrite; CHANGELOG中英成对; README if interface-facing)
- Modify: `docs/roadmap.md` (0.13.1a entry completion note — staged edit per the blob-staging discipline if other sessions' entries persist)
- Test: `tools/manual_sync_check.py` zero drift; manual style suite

- [ ] **Step 1: Version pipeline**

```cmake
if(NOT NLANG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+[a-z]?$")
    ...existing error...
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" NLANG_VERSION_NUMERIC "${NLANG_VERSION}")
project(NLang VERSION ${NLANG_VERSION_NUMERIC} LANGUAGES C CXX)
...
set(CPACK_PACKAGE_VERSION "${NLANG_VERSION}")
```

Verify: configure succeeds; `cpack -C Release` produces `NLang-0.13.1a-win64.zip` naming; nvm `--version` prints 0.13.1a.

- [ ] **Step 2: Docs sync**

语言规格: generics chapter gains free generic functions + call-site inference + `S<int>.f` spelling; static-member chapter lifts the static-T restriction (methods), keeps the static-field compensation pattern; operators page T-op semantics in generic function contexts. User manual: compensation-pattern section rewritten (static fields only). CHANGELOG both languages. `tools/manual_sync_check.py` clean; `pytest tools/nlang-docs/tests/test_manual_style.py -q` green.

- [ ] **Step 3: Commit**

```bash
git add VERSION CMakeLists.txt docs
git commit -m "chore: version 0.13.1a and docs for generic completion"
```

### Task 9: Release sweep

- [ ] **Step 1: Full gate batch** — Debug ctest + Release ctest + IDE ctest + ASan e2e + main e2e + source_size_guard, per the established sweep order (three-tree rebuild parallel, e2e serial; verdict lines from tools/gate.sh only).
- [ ] **Step 2: Final whole-branch review** (fresh reviewer, review-package protocol) → fix pass per executing-plans.
- [ ] **Step 3: Baseline对拍** — e2e case count vs 1878 baseline extended by this version's additions; commit; tag `v0.13.1a` per the annotated-tag ruling; deploy d:/dev/nlang; hash对拍.

## Self-Review Notes (filled at plan time)

- Spec coverage: D1→Task 1; D2→Tasks 1+6; D3→Task 1 (read switch); D4→Tasks 4+5; D5→Tasks 2+3; D6→Task 7; D7→no task (no ABI change — verified nothing in Tasks 1-7 touches NativeHost ABI surfaces); D8→Task 8. 实施顺序 mapping holds; acceptance e2e list distributed across task test files (every spec e2e bullet names its task above).
- The v0.13.1 plan's Task 15 pattern (gate notes) is folded into Task 9.
- Riskiest interface: resolver descIdx-on-invoke-node storage (Tasks 2/4) — follow the OP_New descIdx node-field precedent from v0.13.1 Task 4; if no node field exists, add one SnInvokeExpr field the same way.
