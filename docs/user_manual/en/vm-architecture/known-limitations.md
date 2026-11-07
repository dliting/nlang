# Known Limitations

This page lists the limitations currently visible to user programs,
with workable alternatives where they exist; for language-level
limitations see [Known Limitations](../specification/reference/known-limitations.md).

1. **Exit code range**: The process exit code passes through as the full
   32-bit value; however, POSIX shells keep only the low 8 bits in `$?`
   (an exit code of 300 is observed as 44 under bash). Keep test
   expectations ≤ 255 for cross-observer consistency (see
   [Program Entry and Exit](../specification/09-execution-model/exit-codes.md)).
2. **No struct methods**: Structs are data-only. Use classes for behavior.
3. **`List<int>` storage overhead**: each primitive element is boxed into
   a heap slot (`RTK_Boxed`). For value-heavy workloads, use an `int[]`
   instead — array storage is native (one contiguous allocation of the
   element cells, no per-element boxing).
