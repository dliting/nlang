# Functions


```
int add(int a, int b) {
    return a + b;
}
```

- Parameters: int/float/string/enum passed by value, struct passed by value
  (deep copy), class passed by reference
- Return type: int, float, string, enum, struct (deep copy), class (reference)
- Recursion: supported, with a depth limit (default 1000)

### Void Functions

A function may declare `void` as its return type — it returns no value.
Works for free functions, class methods (any modifier combination, e.g.
`public static void f()`), and interface members (which still require
`public`, like all interface members).

```
void log(int level) {
    if (level == 0) { return; }   // bare `return;` for early exit
}

class Counter {
    public int hits;
    public void bump(int by) { this.hits = this.hits + by; }
}
```

- Bare `return;` exits early; a void function cannot `return expr;`
- A value-returning function cannot use bare `return;` (compile error)
- The result of a void call cannot be consumed — assigning it, using it
  as an operand, or returning it are all compile errors (the call must be
  an expression statement: `log(3);`)
- Void functions combine with out parameters for side-effect-only calls
- Cross-module: imported void stubs carry RTK_Void; the same
  no-result-consumption rules apply on the consumer side
- `void` is not a valid type in any other position (local, field,
  parameter, array element — all rejected at parse time)

### Default Parameters (Phase 9c)

Function parameters may have default values. Defaults can appear at any
position (not just trailing). A default expression may reference earlier
formal parameters.

```
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // references earlier param
int foo(int a, int b = 0, int c) { return c; }     // default not at end
```

- `foo(5)` → `b` gets default value
- `foo(5, 10)` → `b` is 10, default not evaluated
- Default expressions are evaluated at the call site (not at declaration)
- Type mismatch between default expression and parameter type is a compile error

### Named Arguments (Phase 9c)

Arguments may be passed by name using `name = expr` syntax. Positional
arguments must precede named arguments.

```
int foo(int a, int b) { return a * 10 + b; }
foo(a = 5, b = 7);     // named, any order
foo(5, b = 7);          // mixed: positional then named
foo(b = 7, a = 5);      // named, reversed order
```

Errors:
- `foo(b = 2, 1)` — positional after named: compile error
- `foo(1, a = 2)` — duplicate binding for `a`: compile error
- `foo(c = 1)` — unknown parameter name: compile error

### Overload Resolution with Defaults (Phase 9c)

When multiple overloads exist, the compiler selects the best match by
computing a type-distance score. If two or more overloads match with equal
distance, the call is ambiguous and a compile error is reported.

```
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK: second overload (2 args match 2 formals)
foo(5);       // Error: ambiguous (both overloads accept 1 arg)
```

### Out Parameters (Phase 9e)

Parameters declared with `out` are writeback slots: the caller's local
variable is seeded into the callee's frame slot, and after the call
returns, the callee's value is copied back to the caller's variable.

```
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
- The call's return value (pResult) is stored to the result variable
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
- No `out` on constructor (`new`) or `super()` calls.
- No `out` on cross-module imported functions (stub formals lack `NF_Out`).
- At most 32 out parameters per call (outMask is uint32).
- `out` is a reserved keyword.

### Native Functions (Phase 9f)

A function declaration marked `native` has **no body** — the
implementation is provided by the embedding host at runtime:

```
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 — dispatched to the host
}
```

**Model.** The declaration compiles to a function record that carries
only its signature (no bytecode). At the call site the VM looks the
name up in a host-registered table (`VmExecutor::RegisterNative`) and
invokes the native directly with the caller's staged argument cells:

- ABI: argument `i` is the raw 4-byte cell at `args[i*4]` — little-endian
  `int32`/`float` bits or a heap index, identical to the intrinsic ABI.
  The native writes its 4-byte return value into `ret` (may be null for
  `void` natives).
- Calling a native the host never registered throws at the call site
  (`native function not registered: <name>`) — never silent garbage.
- Default parameters work (filled at the call site before dispatch),
  including across module imports (defaults are serialized in v1.6
  modules alongside the native flag).

**Restrictions:**
- A `native` declaration **must not have a body** — compile error.
  The host owns the implementation.
- **No `out` parameters** — writeback needs a callee frame and natives
  have none. Compile error; the VM enforces the same for hand-crafted
  modules.
- The table is keyed by **declaration name only**. The host registration
  is responsible for matching the declared signature; a mismatch (e.g.
  declaring `native string` over an int native) yields garbage output,
  not a type error. Two modules declaring the same native name share one
  table entry.
- Cross-module: a module importing a `.nmod` containing natives calls
  them through the same table (the v1.6 native flag survives the merge).
- Class-member `native` methods work: dispatch reaches the native through
  the normal method path, with `this` riding at `args[0]` (the receiver's
  heap index) followed by the declared parameters — mirroring the bytecode
  calling convention. A host native therefore reads user parameter `j` at
  `args[(1+j)*4]` when bound as a method, but at `args[j*4]` when bound as
  a free function. Registering one implementation under both shapes is a
  signature mismatch (see above).
- String/struct/class argument marshalling beyond the raw 4-byte ABI is
  future work (9f-2).
- **Test host note**: the `ncc` and `nvm` binaries are test hosts — they
  always register `natAdd`, `natConst`, `natFAdd`, and `natPing` so the
  e2e suite can exercise the binding path. A production embedder would
  not include `TestNatives.h`; scripts calling those names against a
  non-test host will get `"native function not registered"` at runtime.

### Frame Layout (Phase 9c follow-up)

Each function's local frame is sized dynamically based on its body:

```
[this?][params][returnSlot][temp1-4][callParamBase(N)][evalArea(peakDepth)][user locals...]
```

- **N** = max callee formal count (plus slot 0 for `this` on methods) observed
  in this function's body. `callParamBase` is the final landing zone consumed
  by `OP_CallFunc`/`OP_CallMethod`.
- **peakDepth** = max simultaneous evalArea slot need across all call sites,
  including nested calls (e.g. `foo(helper(5), helper(10))` needs 4 slots:
  2 for `foo`'s args + 2 for the inner `helper` calls).

The `evalArea` is a disjoint, stack-disciplined staging area. Each
`EmitCallArgs` invocation claims a slice via the `EvalAreaClaim` RAII guard
on entry and releases on exit. Bindings emit to the claimed slice; a
bulk-copy loop then moves them to `callParamBase` just before the call.
This means inner calls' bindings never overwrite outer calls' already-emitted
bindings — fixing the pre-existing `callParamBase` nested-call clobber bug.

A sanity ceiling of 64 formals (`kMaxFuncParams`) prevents unreasonably
large frames; exceeding it is a declaration-time error.
