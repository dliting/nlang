# 7.4 Function Values and Delegates

A function type is written `func<R, P1, ..., Pn>`: the first type
argument is the return type — `void` when the function returns no
value — and the remaining arguments are the parameter types in
order (§2.3). A parameter type argument may carry the `out`
prefix. A function with no parameters is written `func<R>`.

`void` appears as a type argument only in the return slot of
`func<...>`; any other type argument written `void` — such as
`List<void>` — is a compile-time error.

Two function types are the same type when their return types,
parameter types and `out` markers match position by position.
There is no variance: neither the return type nor a parameter type
may differ, even by an implicit conversion (§4.4).

A value of a function type denotes a function: a free function, a
static method, or a method bound to an instance (§5.9). The
default value is `null`; calling a `null` function value is a
run-time error. A function value is a reference type whose only
implicit conversion is to `string` (§4.4): it does not enter
`Object`.

The reference forms — a free function, a static method through its
type name, an instance method bound to a receiver, and a virtual or
interface method late-bound at each call — are §5.9.

A function reference is formed only where a function type is
expected: in a local declaration, an initializer, an assignment, a
return, an argument, an array element or a brace entry (§5.8).
With no expected type the reference is a compile-time error. The
denoted signature must match the expected function type exactly:
no variance, no conversion of the parameters or the result. A
function whose parameters carry defaults cannot be referenced:
defaults are filled only at a direct call (§7.2). Enum methods and
native instance methods cannot be referenced, and a virtual or
interface method with an `out` parameter cannot be referenced
(§7.2).

A bound reference captures its receiver when the value is formed;
state changes made through the reference persist in the receiver
object. A virtual or interface reference dispatches against the
receiver's runtime class at each call (§5.9, §4.8.9, §4.9.4).

A function value is called with `v(a1, ..., an)` (§5.6); a
function-typed field is called as `obj.cb(a1, ..., an)`. A
function-typed local shadows the function of the same name (§3.7).
Named arguments are not accepted on a call through a function
value (§7.2); `out` arguments write back after the call returns
(§7.2). Calling a null function value is a catchable run-time
error (§5.6); forming a bound reference on a null receiver raises a
catchable run-time error at the formation site.

Two function values compare equal under `==` when they denote the
same function and, for a bound reference, carry equal receivers;
`!=` is the complement, and `== null` is allowed. Ordering
comparisons are compile-time errors. A function value renders
through `toString` (§5.3, §4.11.2).

A function value does not enter `Object`. It cannot be a `Dict`
key, a `switch` discriminant, or the target of the serialization
methods (chapter 15).

### Function type syntax

*Informative:* the `func` keyword introduces the function type; the
first type argument is always the return type, the rest are the
parameter types in declaration order, each optionally prefixed with
`out`.

```nlang
func<int>                  // int f()
func<void>                 // void f()
func<int, int>             // int f(int)
func<void, out int>        // void f(out int)
func<int, int, out int>    // int f(int, out int)
```

### Function references

*Informative:* a function or method name **without parentheses** is a
reference — a first-class value that can be stored in locals,
parameters, fields, arrays and containers, passed around, and invoked
later:

- **Free function reference** (`bar`) — a static-bound handle to the
  function.
- **Static method reference** (`Util.twice`) — a receiverless handle
  to a static method; inside the owning class the bare name works too.
- **Bound method reference** (`c.foo`) — captures the receiver: the
  object travels with the handle and its **state persists across
  calls**.
- **Virtual / interface method reference** (`c.tw`, `ifaceVar.run`) — a
  handle that stores the method name and resolves the override chain on
  the receiver's **runtime class at each call** (late binding).

```nlang
import io;
using BinOp = func<int, int>;

class Counter {
    int n;
    public static int twice(int x) { return x * 2; }
    public int foo(int x) { n = n + 1; return n + x; }
    public virtual int tw(int x) { return x * 2; }
}
class Big : Counter {
    public int tw(int x) { return x * 100; }
}
int bar(int x) { return x + 1; }
void apply(BinOp f) { io.print(f(10)); }

int main() {
    Counter c = Counter();
    apply(bar);       // 11    free function
    apply(Counter.twice); // 20    static: no receiver
    apply(c.foo);     // 11    bound: n becomes 1, 1 + 10
    Counter b = Big();
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

*Informative:* two call shapes:

- **Bare identifier** — `f(x)`. A `func`-typed local/parameter/field
  **shadows** any same-named function.
- **Member field** — `obj.cb(x)` where `cb` is a `func`-typed field.

Arguments are staged per the `func` type's signature; `out` parameters
write back to the caller's locals after the call, and the receiver of a
bound method is handled automatically by the call. Named arguments on
delegate invocations are rejected.

### Equality, null, toString

*Informative:* `f == g` / `f != g` compare **handle content** (target +
receiver + form): two references to the same function are equal. `f ==
null` and `f != null` are valid. Ordering comparisons (`< <= > >=`) and
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

### Restrictions

*Informative:* each of the following is a compile-time error:

- Referencing a function/method whose signature does not exactly match
  the expected `func` type.
- Referencing functions or methods with **default parameters** —
  defaults are filled only on the direct-call path.
- **Static method references** — a static method binds as a function
  value through the type name (`Util.twice`), including inherited
  statics, interface statics, native statics, and the bare form inside
  the owning class. Through an object receiver (`b.twice`) it stays
  rejected ("the static method must be called through the type name").
- **Instance methods through the type name** — rejected ("the instance
  method must be called through a receiver object").
- **Enum methods** — the receiver is an int value, not a heap object.
- **Native instance methods** in any reachable form: declared
  `native`, an override under a virtual base, or an implementation of
  an interface member (native calls have no callee frame for the
  receiver). Native static methods are referenceable through the type
  name.
- **Virtual/interface references whose `func` type has `out` parameters**
  — runtime dispatch could disagree with the compiled out mask.
- **`out` in the return position** (`func<out int, ...>`) — rejected
  with the compile error "out is only allowed on func<...>
  parameters.".
- **`func<...>(...)`** — constructing from a name is not supported;
  bind a reference instead.
- **`Dict<func<...>, V>`** — `func` as a Dict key (see the identity note
  above).
- **Boxing into `Object`** (`Object o = f`; see
  [Object and Boxing](../04-types/object.md)) — via the generic
  incompatible-type diagnostic.
- **`void` or `out` type arguments outside `func`** (`List<void>`,
  `List<out int>`).
- Function values as `switch` discriminants (no case family matches).
- Function values cannot cross the serialization methods — `writeStruct`
  / `writeObject` on a struct/class holding a `func` field is a run-time
  error (chapter 15).
