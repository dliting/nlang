# Stack Frame Layout


Each function invocation has a local variable frame:

```
[this (methods)] [params...] [returnSlot] [tempSlot] [tempSlot2] [callParamBase(8 slots)] [user locals...]
```

All slots are 4 bytes (VALUE_SIZE). The frame is a flat byte array indexed
by offset. `LocalDescriptor` records each variable's offset, size, and
typeKind for GC root scanning.

### Temp Slots

- **tempSlot**: Used for binary op right operand, condition evaluation, and
  intermediate values in member access chains.
- **tempSlot2**: Second temp for nested binary expressions. When resultOffset
  == tempSlot, the right operand goes to tempSlot2 to avoid overwriting.

### Call Parameter Area

8 slots (32 bytes) at `callParamBase`. For method calls, slot 0 is `this`.
Arguments are evaluated left-to-right into this area before the call.
