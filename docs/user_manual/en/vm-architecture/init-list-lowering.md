# Init List Lowering

Collection initializers let developers populate an array or container
with a single literal expression, such as `[1,2,3]` or `Type{...}`.
These literals have no runtime form of their own: the compiler lowers
them into ordinary allocation and store instructions. This page
describes the lowering path, how the target type is inferred, and how
the container branches handle boxing.

Collection initializers (`[1,2,3]`, `Type{...}`) lower through a
single `NK_InitListExpr` handler in `VmBackend::EmitExpression`. The
handler dispatches on the resolved `EvalDataType` and the node's
`IsArrayValued()` predicate — an array-form initializer carries the
interned array token in its type slot, so array-ness is part of the
type itself.

### Resolver data flow

`StatementResolveAccessor::Access(SnAssignStmt&)` peeks at the RHS
before resolving it: a bare init list (no `ExplicitType`) records the
LHS field on the node via `InferredTarget(pLeftField)` — member stores
(`b.m = {...}`) included. Every position that can supply a target
propagates it the same way: a bare brace return value takes the
function's declared return type, a subscript-store RHS takes the
element / container value-slot type, a call argument takes the formal's
type, and a foreach iterable takes the loop-variable type (the brace
form iterates a `List<T>` minted from it; the bracket form takes the
interned array token).

The resolver then visits the RHS; `ExprResolveAccessor::Access(SnInitListExpr&)`
reads either the explicit `ExplicitType()` or the `InferredTarget()` —
with neither, a bracket-form literal infers its element type from the
entries (all entries must share one type) — and sets `EvalDataType` to
the resolved target type: for array targets the interned array token
(`InternArrayTypeToken(element)`), so the node is array-valued by
construction and consumers peel the element with `ElemTypeOf()`.

The resolver propagates the element target into child init lists per
slot (`PropagateNestedInitListTargets`), so nested bare literals infer
their type — `[[1,2],[3]]` and `{{1,2},{3}}` both work.

### Codegen dispatch table

| Target kind                 | Allocation             | Per-entry store                      |
|-----------------------------|------------------------|--------------------------------------|
| `T[]` array                 | `OP_AllocArray` size=N | `OP_ConstInt32 i; OP_StoreElement`  |
| `List<T>`                   | `OP_New "List"` + ctor | `OP_Box` (if T primitive); `OP_CallMethod "add"` |
| `Dict<K,V>`                 | `OP_New "Dict"` + ctor | key and value as expressions; `OP_Box` (if K/V primitive); `OP_CallMethod "set"` |
| `Channel<T>`                | `OP_New "Channel"` + ctor (unbounded capacity staged as a constant) | — (the resolver rejects entries; values enter through `send`) |
| user class                  | `OP_New` + no-arg ctor | `OP_StoreField <offset>`            |
| struct                      | `OP_AllocStruct`       | `OP_StoreField <offset>`            |

### Per-method boxing plan reuse

The `List<T>` and `Dict<K,V>` branches reuse the per-method boxing
infrastructure of container calls: `BoxingTagFor(typeArg)` returns
`{tag, isPrimitive}`, and `OP_Box` is emitted before `add`/`set` only
when the element type is primitive **and not an array type** — an
array-typed type argument (`List<int[]>`, `Dict` value `V[]`) flows as
a raw handle with no boxing, the same exception as hand-written
`lst.add(x)` calls. This keeps initialization consistent with
`lst.add(x)` and `d.set(k, v)` calls written by hand.

### Staging of entry values

The handler writes the collection to `resultOffset` (provided by the
caller, e.g. `tempSlot2` for struct-assignment codegen). Each entry's
value is evaluated into a staging slot of the evaluation scratch area:
the array, class, and struct arms span the entry loop with one
`EvalAreaClaim`; the List and Dict arms claim per-entry slots for
`{this, value}` and `{this, key, value}`. Each entry is consumed
immediately after it is evaluated, and nested init lists (which
recursively enter this handler) claim fresh slots above, so they do
not clobber the parent's value slot.

### Reuse of existing opcodes

Init-list lowering adds no new opcodes. Allocation, element stores,
method calls, and field stores all reuse the existing primitives.
The init-list handler is pure orchestration: allocate once, then
populate via the existing stores.
