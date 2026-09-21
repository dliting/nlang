# Known Limitations

This page lists the limitations currently visible to user programs,
with workable alternatives where they exist.

1. **Exit code range**: Process exit codes are 8-bit (0-255) on Windows.
   Test values must not exceed 255.
2. **No super() call**: Ancestor constructors are not automatically invoked.
3. **No struct methods**: Structs are data-only. Use classes for behavior.
4. **No user-defined generics**: `class Foo<T> { ... }` is not supported.
   Only built-in generic classes (`List<T>`, `Dict<K,V>`) are
   recognized by the compiler.
5. **`List<int>` storage overhead**: each primitive element is boxed into
   a heap slot (`RTK_Boxed`). For value-heavy lists, an `IntList`
   specialization with unboxed storage is the planned escape hatch
   (deferred until profiling shows real need).
6. **`List<struct>` value semantics**: adding the same struct variable
   twice shares the underlying heap slot (boxing is reference semantics).
   Use separate struct instances for distinct elements.
