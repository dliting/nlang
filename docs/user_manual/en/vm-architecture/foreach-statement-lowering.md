# Foreach Statement Lowering

`foreach` lets developers iterate arrays, containers, and strings
without managing indices by hand. At the VM level it has no dedicated
opcode (the string arm excepted, see below): the compiler lowers
`foreach (Type var in iterable) { body }` to **index-based expansion**
for Array/List/Dict — the same shape as the `for` loop with a
body-prelude that loads element `i` into the user variable slot — and
to a dedicated code-point expansion for strings.

### Hidden locals (uniquified for nesting)

Each index-based `foreach` allocates four hidden locals before the
`LoopContext` push, named with a per-function counter to avoid
`AllocLocal`'s dedup-by-name collision in nested loops:

| Local | typeKind | Purpose |
|-------|----------|---------|
| `<varName>` | derived from element type — `RTK_Array` when the element is array-typed | user-visible loop variable |
| `__foreach_iter_<N>` | `RTK_Array` (Array) or `RTK_Class` (List/Dict) | iterable reference |
| `__foreach_i_<N>` | `RTK_Int32` | loop counter |
| `__foreach_n_<N>` | `RTK_Int32` | cached length |

`<N>` comes from `FuncContext::foreachCounter`, which is reset to 0 at
function entry. The string arm allocates a different trio instead:
`__foreach_iter_<N>` (`RTK_String`), `__foreach_off_<N>` (`RTK_Int32`,
byte offset) and `__foreach_cond_<N>` (`RTK_Int32`, continue flag),
with the loop variable typed `RTK_Char`.

### Codegen 4-way branch

The kind of iterable is classified at codegen time (not resolver time),
preserving the user-visible abstract syntax tree (AST):

- **Array** (`T[N]`): detected by two arms — the declaration-side arm
  (identifier or member lvalue whose `Field` has `IsArrayType()`) and
  the value-form arm (any expression whose `EvalDataType()` carries the
  interned array type token: `get()` and subscript reads out of
  `List<T[]>`/`Dict<K[],V>`, array-returning calls, new-array
  expressions). Length via `OP_ArrayLength`; element via
  `OP_LoadElement` (with `OP_CopyStruct` for struct-element types,
  value semantics on read).
- **List<T>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="List"` and `IsGenericInstantiation()`. Length via
  `OP_CallMethod "length"`; element via `OP_CallMethod "get"` followed
  by `OP_Unbox` for primitive T — skipped when the type argument is an
  array type (`List<int[]>` elements flow as raw handles; per-method
  boxing plan, array-argument exception).
- **Dict<K,V>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="Dict"`. **Inline `keys()` call** materializes a fresh
  `List<K>` into `iterSlot` first, then the rest mirrors the
  List path with element type K.
- **string**: iterates Unicode code points. The loop head *is* the step
  — one `OP_StrForeachStep` decodes the code point at the byte offset
  into the loop variable, advances the offset by the encoded length,
  and writes the continue flag; `OP_JumpIfNot` on that flag ends the
  loop. `continue` jumps back to the head (the step is the increment),
  `break` to the loop end. There is no null check: a null source reads
  as the empty string and the first step clears the flag.

The `typeKind` of each hidden local is what garbage collection (GC) uses at safepoints to
identify reference roots, so `iterSlot` must be `RTK_Array` for the
Array path and `RTK_Class` for List/Dict — incorrect tags would cause
either leaked references (root missed) or spurious tracing of integer
slots as heap idxs. The same applies to the user-visible loop variable:
an array-typed loop variable (`foreach (int[] row in grid)`) allocates
its slot as `RTK_Array`, so the handle each iteration binds is a traced
root.

### `Dict.keys()` intrinsic

`INTR_Dict_Keys = 60` (include/nlang/vm/Intrinsics.h). Registered as a
method named `keys` on the Dict class with `paramCount=1,
returnTypeKind=RTK_Class`. The VmExecutor handler:

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
the same break/continue patch logic. In the index-based expansion
`continue` jumps to the post-body "i = i + 1" site; in the string
expansion it jumps to the head (the step advances the offset). `break`
jumps to the loop end in both.
