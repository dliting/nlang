# Out Parameters


Parameters declared with `out` are writeback slots: the caller's local
variable is copy-initialized into the callee's frame slot, and after the call
returns, the callee's value is copied back to the caller's variable.

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
- The writeback occurs after the call returns, in slot order.
- The call's return value is stored to the result variable
  **before** out writebacks, so `int x = f(out x)` reads the old `x`
  for the return assignment and then overwrites `x` with the callee's
  output.

**Restrictions:**
- Out arguments must be **local variables** (not fields, array elements,
  or arbitrary expressions). Compile error otherwise.
- An out parameter may be any scalar or class type; a **struct is the one
  exclusion** (`out parameter "rem" cannot be a struct`) — structs have
  variable-size value semantics incompatible with the fixed-slot
  writeback mechanism.
- An out parameter cannot carry a default value.
- No `out` parameters on native functions (`native function "f" cannot
  have out parameters`) or enum methods (`enum method "m" cannot have
  out parameters`).
- No `out` arguments on built-in (standard-library) method calls
  (`out arguments are not supported by built-in methods`).
- No `out` on virtual/interface dispatch calls — the writeback mask is
  baked against the statically bound callee's layout (`out arguments are
  not supported on virtual method "m".`).
- No `out` on constructor calls or `super()` (`out arguments are not
  supported in constructor calls.`).
- At most 32 out parameters per call.
- `out` is a reserved keyword.
