# Foreach Statement Lowering (Phase 8e-5)


`foreach (Type var in iterable) { body }` compiles to **index-based
expansion** — no new opcode is introduced. The 12-step lowering mirrors
the `for` loop pattern (line 1991 of VmBackend.cpp) with a body-prelude
that loads element `i` into the user variable slot.

### Hidden locals (uniquified for nesting)

Each `foreach` allocates four hidden locals before the `LoopContext`
push, named with a per-function counter to avoid `AllocLocal`'s dedup-
by-name collision in nested loops:

| Local | typeKind | Purpose |
|-------|----------|---------|
| `<varName>` | derived from element type | user-visible loop variable |
| `__foreach_iter_<N>` | `RTK_Array` (Array) or `RTK_Class` (List/Dict) | iterable reference |
| `__foreach_i_<N>` | `RTK_Int32` | loop counter |
| `__foreach_n_<N>` | `RTK_Int32` | cached length |

`<N>` comes from `FuncContext::foreachCounter`, which is reset to 0 at
function entry.

### Codegen 3-way branch

The kind of iterable is detected at codegen time (not resolver time),
preserving the user-visible AST:

- **Array** (`T[N]`): iterable is `SnIdentifierExpr` whose `Field` has
  `IsArrayType()`. Length via `OP_ArrayLength`; element via
  `OP_LoadElement` (with `OP_CopyStruct` for struct-element types).
- **List<T>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="List"` and `IsGenericInstantiation()`. Length via
  `OP_CallMethod "Length"`; element via `OP_CallMethod "Get"` followed
  by `OP_Unbox` for primitive T (per-method boxing plan).
- **Dict<K,V>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="Dict"`. **Inline `Keys()` call** materializes a fresh
  `List<K>` into `iterSlot` first (step 2b), then the rest mirrors the
  List path with element type K.

The `typeKind` of each hidden local is what GC uses at safepoints to
identify reference roots, so `iterSlot` must be `RTK_Array` for the
Array path and `RTK_Class` for List/Dict — incorrect tags would cause
either leaked references (root missed) or spurious tracing of integer
slots as heap idxs.

### `Dict.keys()` intrinsic

`INTR_Dict_Keys = 60` (CompiledModule.h). Registered as a method on the
Dict class with `paramCount=1, returnTypeKind=RTK_Class`. The VmExecutor
handler:

1. Reads the dict handle via `ReadDictHandle` (uniform null/stale/uninit
   validation).
2. `AllocClassOnHeap(m_listClassIdx)` — fresh List class instance.
3. `AllocListHandle()` — fresh List side-table slot.
4. Wires the handle into `m_structHeap[heapIdx][kListHandleFieldOffset]`.
5. Copies `dict.entries[i].first` (the K heap idxs) into
   `m_listStore[handle-1].elements`.
6. Writes `listHeapIdx` to `pResult`; sets `m_gcPending = true`.

The resulting `List<K>` is traced automatically by the existing GC
`MarkPhase` (it already handles `classIdx == m_listClassIdx`). The K
heap idxs already exist in the dict and are tracked through it; the new
List holds additional references to the same objects, which is safe
(GC mark bits dedupe).

### LoopContext reuse

`m_loopStack` already supports `break`/`continue` for `for`/`while`/`do`
and `switch`. `foreach` pushes a `LoopContext{isSwitch=false}` and uses
the same break/continue patch logic. `continue` jumps to the post-body
"i = i + 1" site; `break` jumps to the loop end.
