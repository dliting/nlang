# Known Limitations

This page lists the limitations currently visible to user programs,
with workable alternatives where they exist; for language-level
limitations see [Known Limitations](../language-spec/known-limitations.md).

1. **Exit code range**: The process exit code passes through as the full
   32-bit value; however, POSIX shells keep only the low 8 bits in `$?`
   (an exit code of 300 is observed as 44 under bash). Keep test
   expectations ≤ 255 for cross-observer consistency (see
   [Exit Code Conventions](../language-spec/exit-code-convention.md)).
2. **No struct methods**: Structs are data-only. Use classes for behavior.
3. **No user-defined generics**: `class Foo<T> { ... }` is not supported.
   Only built-in generic classes (`List<T>`, `Dict<K,V>`) are
   recognized by the compiler.
4. **`List<int>` storage overhead**: each primitive element is boxed into
   a heap slot (`RTK_Boxed`). For value-heavy lists, an `IntList`
   specialization with unboxed storage would remove the boxing.
5. **`List<struct>` value semantics**: adding the same struct variable
   twice shares the underlying heap slot (boxing is reference semantics).
   Use separate struct instances for distinct elements.
