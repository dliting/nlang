# Stack Frame Layout


Each function invocation has a local variable frame:

```text
[this (methods)] [params...] [returnSlot] [tempSlot..tempSlot4] [callParamBase(N)] [evalArea(peakDepth)] [user locals...]
```

All slots are 4 bytes (VALUE_SIZE). The frame is a flat byte array indexed
by offset. `LocalDescriptor` records each variable's offset, size, and
typeKind for GC root scanning.

### Temp Slots

`tempSlot..tempSlot4` form a 4-slot pure scratch pool: each use is
consumed immediately and never survives across emissions. Operands that
must stay live across an emission (arguments of nested calls, intermediate
receivers in member chains) do not use the pool — they claim slices of the
evaluation scratch area via `EvalAreaClaim` and release them by lexical
scope.

### Call Parameter Area

N slots starting at `callParamBase`. N is the largest callee formal count
seen anywhere in this function's body (minimum 1), computed at compile
time by `ComputeCallSlotStats`. For method calls, slot 0 is `this`.
Arguments are evaluated left-to-right into this area before the call.

### Evaluation Scratch Area

`peakDepth` slots starting at `evalArea`. peakDepth is the maximum number
of scratch slots any call site (including nested calls) needs
simultaneously, also computed at compile time. The area follows stack
discipline as a whole: bindings of an inner call never overwrite
arguments already emitted by an outer call.
