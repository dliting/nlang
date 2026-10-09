# Function Types & Delegates


Function types are introduced by the `func` keyword:
`func<Return, Params...>` describes **function values**. The first type
argument is always the **return type** (may be `void`); the remaining
arguments are the parameter types in declaration order, each optionally
prefixed with `out`:

```nlang
func<int>                  // int f()
func<void>                 // void f()
func<int, int>             // int f(int)
func<void, out int>        // void f(out int)
func<int, int, out int>    // int f(int, out int)
```

Signature matching is **exact** — no co-/contravariance, no default
filling.

### Function References

A function or method name **without parentheses** is a reference — a
first-class value that can be stored in locals, parameters, fields,
arrays and containers, passed around, and invoked later:

- **Free function reference** (`bar`) — a static-bound handle to the
  function.
- **Bound method reference** (`c.foo`) — captures the receiver: the
  object travels with the handle and its **state persists across
  calls**.
- **Virtual / interface method reference** (`c.tw`, `ifaceVar.run`) — a
  handle that stores the method name and resolves the override chain on the receiver's
  **runtime class at each call** (late binding).

```nlang
using BinOp = func<int, int>;

class Counter {
    int n;
    int foo(int x) { n = n + 1; return n + x; }
    virtual int tw(int x) { return x * 2; }
}
class Big : Counter {
    int tw(int x) { return x * 100; }
}
int bar(int x) { return x + 1; }
void apply(BinOp f) { io.print(f(10)); }

int main() {
    Counter c = new Counter();
    apply(bar);       // 11    free function
    apply(c.foo);     // 11    bound: n becomes 1, 1 + 10
    Counter b = new Big();
    apply(b.tw);      // 1000  virtual: runtime class Big wins
    BinOp g = c.foo;
    return g(1) + g(1);   // state persists: 3 + 4 = 7
}
```

Binding happens at **consumer sites** where an expected function type
exists: declaration/assignment right-hand sides (including fields and
subscript stores), return positions, call arguments, container-method
arguments (`l.add(bar)`), and init-list entries (`[bar]`). A reference
with no expected function type is a compile error naming the reference.

### Invocation

Two call shapes:

- **Bare identifier** — `f(x)`. A `func`-typed local/parameter/field
  **shadows** any same-named function.
- **Member field** — `obj.cb(x)` where `cb` is a `func`-typed field.

Arguments are staged per the `func` type's signature; `out` parameters
write back to the caller's locals after the call, transparently
handling the receiver shift for bound methods. Named arguments on
delegate invocations are rejected.

### Equality, null, toString

- `f == g` / `f != g` compare **handle content** (target + receiver +
  form): two references to the same function are equal. `f == null`
  and `f != null` are valid. Ordering comparisons (`< <= > >=`) and
  comparisons with non-`func` operands are compile errors.
- `f = null` stores the empty handle; **invoking it throws a null-
  pointer exception** at run time. Binding a reference on a **null
  receiver** (`Counter c = null; register(c.tw);`) throws at bind time.
- `f.toString()`, `f as string`, `"" + f`, `io.print(f)`, and container
  formatting render `"func <name>"` (static handles — including bound
  non-virtual method references) or `"method <name>"` (virtual-dispatch
  handles).
- **Known inconsistency**: `List<func<...>>.contains` / `indexOf` compare
  elements by **identity** (each reference is a distinct heap record),
  not by `==` content equality.

### Restrictions (compile-time)

- Referencing a function/method whose signature does not exactly match
  the expected `func` type.
- Referencing functions or methods with **default parameters** —
  defaults are filled only on the direct-call path.
- **Enum methods** — the receiver is an int value, not a heap object.
- **Native methods** in any reachable form: declared `native`, an
  override under a virtual base, or an implementation of an interface
  member (native calls have no callee frame for the receiver).
- **Virtual/interface references whose `func` type has `out` parameters**
  — runtime dispatch could disagree with the compiled out mask.
- **`out` in the return position** (`func<out int, ...>`) — rejected
  with the compile error "out is only allowed on func<...>
  parameters.".
- **`new func<...>(...)`** — constructing from a name is not supported;
  bind a reference instead.
- **`Dict<func<...>, V>`** — `func` as a Dict key (see identity note above).
- **Boxing into `Object`** (`Object o = f`) — via the generic
  incompatible-type diagnostic.
- **`void` or `out` type arguments outside `func`** (`List<void>`,
  `List<out int>`, `new List<out int>`).
- **Cross-module**: referencing an imported function, or passing a
  function reference **to** an imported function (parameter signatures
  are not serialized in `.ncu`).
- Function values as `switch` discriminants (no case family matches).
- Function values cannot cross the serialization application programming interface (API) — `writeStruct` /
  `writeObject` on a struct/class holding a `func` field is a run-time
  error.
