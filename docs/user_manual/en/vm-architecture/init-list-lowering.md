# Init List Lowering

Collection initializers let developers populate an array or container
with a single literal expression, such as `[1,2,3]` or `new Type{...}`.
These literals have no runtime form of their own: the compiler lowers
them into ordinary allocation and store instructions. This page
describes the lowering path, how the target type is inferred, and how
the container branches handle boxing.

Collection initializers (`[1,2,3]`, `new Type{...}`) lower through a
single `NK_InitListExpr` handler in `VmBackend::EmitExpression`. The
handler dispatches on the resolved `EvalDataType` plus the
`TargetIsArray` flag set by the resolver.

### Resolver data flow

`StatementResolver::Access(SnAssignStmt&)` peeks at the RHS before
resolving it. If the RHS is a bare `SnInitListExpr` (no `ExplicitType`)
without an `InferredTarget`, the LHS variable is recorded on the init
list via `InferredTarget(pLeftField)`. The resolver then visits the
RHS; `ExprResolveAccessor::Access(SnInitListExpr&)` reads either the
explicit `ExplicitType()` or the `InferredTarget()`, sets
`EvalDataType` to the resolved target field, and copies array-ness
into `TargetIsArray` (needed because `EvalDataType` on an array
variable returns the element type — the array-ness would otherwise be
lost).

The resolver does not propagate expected types to child init lists
(recursive type inference for `[[1,2],[3]]` is not yet supported).
Children must use the explicit `new Type{...}` form to carry their
own type.

### Codegen dispatch table

| Target kind                 | Allocation             | Per-entry store                      |
|-----------------------------|------------------------|--------------------------------------|
| `T[]` array                 | `OP_AllocArray` size=N | `OP_ConstInt32 i; OP_StoreElement`  |
| `List<T>`                   | `OP_New "List"` + ctor | `OP_Box` (if T primitive); `OP_CallMethod "Add"` |
| `Dict<K,V>`                 | `OP_New "Dict"` + ctor | `OP_ConstString key`; `OP_Box` (if V primitive); `OP_CallMethod "Set"` |
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

### Temp slot allocation

The handler writes the collection to `resultOffset` (provided by the
caller, e.g. `tempSlot2` for struct-assignment codegen). Each entry's
value is evaluated to a distinct slot chosen by `PickTempSlot(resultOffset)`
so that nested init lists (which recursively enter this handler) do not
clobber the parent's value slot.

### Reuse of existing opcodes

Init-list lowering adds no new opcodes. Allocation, element stores,
method calls, and field stores all reuse the existing primitives.
The init-list handler is pure orchestration: allocate once, then
populate via the existing stores.
