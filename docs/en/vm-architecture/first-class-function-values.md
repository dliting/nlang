# First-Class Function Values (Phase 13)


A function value (delegate) is a **3-slot heap record** with
`m_slotKinds[idx] == RTK_Func`:

| slot | static-bound handle (`kFuncFormStatic`) | virtual-dispatch handle (`kFuncFormVirtual`) |
|------|------------------------------------------|----------------------------------------------|
| [0]  | `functions[]` index                      | string-pool index (method name)              |
| [1]  | receiver heap index (0 for free functions) | receiver heap index                        |
| [2]  | form (0 / 1)                             | form (0 / 1)                                 |

**Invariant**: `slot[1] == 0` ⟺ free-function form. The bind-time
null-receiver guard in OP_MakeBoundFunc / OP_MakeVFunc (throws a null-
pointer exception when the receiver slot reads ≤ 0) establishes it: a null
receiver can never materialize a handle that would take the free-function
dispatch path with a garbage frame.

### Dispatch layout (the `this == 0` branch)

`ExecuteDelegateCall` (shared by OP_CallDelegate / OP_CallDelegateOut)
branches on `slot[1]`:

- **Free function** — args are copied verbatim from `callParamBase`
  (the OP_CallFunc ABI). Native targets read the caller's cells directly.
- **Bound method** — the captured receiver occupies callee frame slot 0
  and the caller's args are staged WITHOUT this, landing in slots 1+
  (`callee.paramCount` includes this for methods).
- **Virtual-dispatch handle** — the target is resolved first, by method
  name on the receiver's runtime class (`FindMethodByName`, the same
  lookup OP_CallMethod performs, walking `methodIndices` up the
  `superClassIdx` chain), then executed with the bound-method layout.
  Native or intrinsic methods landing here throw at dispatch time
  (the resolver already rejects the compilable shapes).

Out-argument writeback (`OP_CallDelegateOut`) reverses the shift:
outMask bit *i* marks USER parameter *i* in Func-signature order; the
write-back reads frame slot `i+1` for bound handles (slot `i` for free
functions) and stores to `callParamBase + i`.

### Receiver-first emission

OP_MakeBoundFunc / OP_MakeVFunc read the receiver from `pResult` BEFORE
writing the handle: codegen emits the receiver expression into the
result slot, refreshes `pResult`, then the bind opcode reads it and
overwrites the slot with the handle index.

### GC trace sites

MarkPhase gates the trace sites on kind — most on the runtime kind,
the two root scans on the static local/return kinds; `RTK_Func` records
are traced at **eight** sites (Phase 13 added the arms across the board):

1. root-set scan (locals/params with `LocalDescriptor.typeKind == RTK_Func`)
2. frame `pResult` scan (in-progress return values)
3. heap-object pop dispatch (the receiver in `slot[1]`)
4. class field scan (static `fieldTypeKinds` + runtime kind)
5. struct field scan (shallow copy on struct assignment is the correct
   semantics — the handle is a reference, not a value)
6. array element scan (`elemKind`)
7. List element scan
8. Dict entry scan

Handles never enter boxed slots (boxed slots are untraced). Allocation
always happens — no interning — so two references to one function are
distinct records compared by content (`OP_Eq_func`).
