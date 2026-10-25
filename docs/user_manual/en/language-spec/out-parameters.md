# Out Parameters


A parameter declared with `out` writes its result back to the caller:
the argument must be a local variable, and after the call returns, the
callee's assignments to the parameter are written back to that variable.

```nlang
int divide(int a, int b, out int rem) {
    rem = a % b;
    return a / b;
}
int r = 0;
int q = divide(17, 5, out r);  // q=3, r=2
```

**Semantics:**
- Out parameters are **inout**: the callee sees the caller's current value
  as the initial value of the parameter local.
- The writeback occurs after the call returns.
- The call's return value is stored to the result variable
  **before** out writebacks, so `int x = f(out x)` reads the old `x`
  for the return assignment and then overwrites `x` with the callee's
  output.

**Restrictions:**
- Out arguments must be **local variables** (not fields, array elements,
  or arbitrary expressions). Compile error otherwise.
- Out parameters must be **4-byte scalar or class-typed** (no struct).
  Structs have variable-size value semantics incompatible with the
  fixed-slot writeback mechanism.
- No `out` on virtual/interface dispatch calls (the concrete callee is
  unknown at compile time).
- No `out` on constructor calls or `super()`.
- At most 32 out parameters per call.
- `out` is a reserved keyword.
