# 7.4 Function Values and Delegates

A function type is written `func<R, P1, ..., Pn>`: the first type
argument is the return type — `void` when the function returns no
value — and the remaining arguments are the parameter types in
order (§2.3). Parameter and return types may carry the `&` marker
(`func<int&, int&>` — a reference parameter and a reference
return, §4.7.9). A function with no parameters is written
`func<R>`.

`void` appears as a type argument only in the return slot of
`func<...>`; any other type argument written `void` — such as
`List<void>` — is a compile-time error.

Two function types are the same type when their return types,
parameter types and `&` markers match position by
position. There is no variance: neither the return type nor a
parameter type may differ, even by an implicit conversion (§4.4).
A `func` type with `&` and its unmarked form are different types:
binding an unmarked function to a marked expectation (or the other
way round) is a compile error
(`function "plain" does not match the signature of "func<Int32, Int32&>"`).
The diagnostic spells the markers position by position.

A function value with `&` markers references and calls as usual:
at the call, a `&` argument position follows the same bindable-
storage rules as a direct call (§7.2), and writes through the
function value land in the caller; a `&` return passes the
callee's reference through to the binding side.

A value of a function type denotes a function: a free function, a
static method, or a method bound to an instance (§5.9). A generic
function can be referenced too — its value denotes one
instantiation of the function: the type arguments are inferred
from the expected function type, or named by the qualified
instantiation spelling (`Tag<int>.tag`, §4.12). The
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
native instance methods cannot be referenced.

A bound reference captures its receiver when the value is formed;
state changes made through the reference persist in the receiver
object. A virtual or interface reference dispatches against the
receiver's runtime class at each call (§5.9, §4.8.9, §4.9.4).

A function value is called with `v(a1, ..., an)` (§5.6); a
function-typed field is called as `obj.cb(a1, ..., an)`. A
function-typed local shadows the function of the same name (§3.7).
Named arguments are not accepted on a call through a function
value (§7.2). Calling a null function value is a catchable run-time
error (§5.6); forming a bound reference on a null receiver raises a
catchable run-time error at the formation site.

Two function values compare equal under `==` when they denote the
same instantiation of the same function — a generic function's
instantiation participates in equality: two values of one function
with different instantiations are not equal — and, for a bound
reference, carry equal receivers; closures compare by closure
identity — two evaluations of one lambda literal in a single
activation of its scope are equal (see Lambda expressions below);
`!=` is the complement, and `== null` is allowed. Ordering
comparisons are compile-time errors. A function value renders
through `toString` (§5.3, §4.11.2).

A function value does not enter `Object`. It cannot be a `Dict`
key, a `switch` discriminant, or the target of the serialization
methods (chapter 14).

### Function type syntax

*Informative:* the `func` keyword introduces the function type; the
first type argument is always the return type, the rest are the
parameter types in declaration order.

```nlang
func<int>                  // int f()
func<void>                 // void f()
func<int, int>             // int f(int)
func<int, int&>            // int f(int&)
func<int&, int&>           // int& f(int&)
```

### Function references

*Informative:* a function or method name **without parentheses** is a
reference — a first-class value that can be stored in locals,
parameters, fields, arrays and containers, passed around, and invoked
later:

- **Free function reference** (`bar`) — a static-bound handle to the
  function.
- **Static method reference** (`Util.twice`) — a receiverless handle
  to a static method; inside the owning class the unqualified name works too.
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

### Lambda expressions

*Informative:* `->` introduces a lambda expression — a function
value written in place. The parameter list admits three kinds of
entry: bare identifiers (`(x, y)` — the type and `&` marker come from
the corresponding position of the expected function type), typed
identifiers (`(int& x)` — must match the expected type exactly
position by position, no variance), and the empty list (`()` — a
zero-parameter lambda). The body takes two forms: an expression body
whose value is the return value; a block body is a brace-wrapped
statement sequence returning through `return` — no `return` is
needed under a `func<void>` expectation, and a block body that
returns no value under any other expectation is a compile error.

```nlang
import io;

int main() {
    func<int, int> add = (x) -> x + 1;
    func<void> hello = () -> { io.print("hello"); };
    func<int&, int&> id = (int& x) -> x;
    hello();                  // hello
    return add(41);           // 42
}
```

A lambda shares the same consumer positions as a function reference:
declarations, assignments, returns, arguments, array elements, brace
entries. A lambda with no expected function type is a compile-time
error; so is a parameter count or an explicit parameter type that
disagrees with the expectation.

**Capture.** An outer local visible in the lambda body (parameters
included) is captured when referenced: the variable's storage moves
to the heap, the code inside and outside the lambda shares the one
storage, and a write on either side is visible to the other. Multiple
capture points of one variable — and the outer reads and writes —
all land on the same storage.

```nlang
int main() {
    int count = 0;
    func<int> bump = () -> { count = count + 1; return count; };
    func<int> peek = () -> count;
    bump();                   // count = 1
    bump();                   // count = 2
    return peek();            // 2 — the two lambdas share one storage
}
```

Capture does not change the declared semantics of the captured
variable: assigning to a captured `const` local inside the lambda is
still a compile error. A `static` local is not captured — a reference
to it in the lambda body addresses the static slot directly, shared
by all calls (§6.1). Referencing an outer `static` local inside a
lambda body is legal; declaring a new `static` local stays bound by
the §6.1 edge (declarable in a function or method body, not in a
generic context). A `catch` clause's variable is an ordinary local in
this respect — a lambda in the handler captures the exception object
the clause names. Accessing an instance member in the lambda body
captures the receiver; referencing `this` in a static context is a
compile error. An `&` reference (a reference local, a reference
parameter, a `foreach` reference head) captures the binding itself:
reads and writes inside the lambda go through to the referent's
storage, and the referent's lifetime safety is a language guarantee —
every `&` binding targets heap storage.

**Loop-head freshness.** A variable declared in a `for` or `foreach`
head is a new variable on each iteration: lambdas captured in
different iterations each hold that iteration's value, not a shared
tail value after the loop ends.

**Reference parameters and reference returns.** A lambda under a
`func<int&, int&>` expectation may declare `&` parameters and an `&`
return: writes through a `&` parameter land in the caller's storage;
returning a reference to a captured variable is legal — its storage
is already on the heap and stays valid after the call.

**Recursion.** A lambda can call itself recursively through the
local it is bound to: that local is captured before the lambda is
created, and reading it in the body reads the captured storage.

```nlang
int main() {
    func<int, int> fact = (n) -> { if (n <= 1) { return 1; } return n * fact(n - 1); };
    return fact(5);           // 120
}
```

**Generic context.** A lambda inside a generic function or a generic
class method body can capture variables of a type-parameter type:
the captured storage carries the real runtime instantiation, and
`T` operations in the lambda body dispatch on the actual type —
`typeName(T)` and `==` comparisons behave the same as outside a
generic function body.

**Closure identity.** A closure value consists of the lambda method
and its captured storage. Evaluating one lambda literal twice within
a single activation of its scope yields **equal** function values;
re-activating the scope — calling the containing function again,
entering the next loop iteration — yields **unequal** closures. A
zero-capture lambda follows the same rule. An "unequal" assertion
therefore wants closures from two activations; two evaluations in
one activation are always equal.

A closure's `toString` renders as `"lambda"`; the `==`/`!=` and
`null` rules are those of function references (see below).
`thread.spawn` accepts a zero-parameter lambda as a spawn target
(§10.1).

### Invocation

*Informative:* three call shapes:

- **Bare identifier** — `f(x)`. A `func`-typed local/parameter/field
  **shadows** any same-named function.
- **Member field** — `obj.cb(x)` where `cb` is a `func`-typed field.
- **Static field** — `Cfg.dbl(x)` where `dbl` is a `func`-typed static
  field; the type name qualifies, no receiver exists, and the field's
  current handle is invoked.

Arguments are staged per the `func` type's signature, and the receiver
of a bound method is handled automatically by the call. Named arguments
on delegate invocations are rejected.

### Equality, null, toString

*Informative:* `f == g` / `f != g` compare **handle content** (target +
receiver + form + instantiation descriptor): two references to the
same function and instantiation are equal. `f ==
null` and `f != null` are valid. Ordering comparisons (`< <= > >=`) and
comparisons with non-`func` operands are compile errors.

- `f = null` stores the empty handle; **invoking it throws a null-
  pointer exception** at run time. Binding a reference on a **null
  receiver** (`Counter c = null; register(c.tw);`) throws at bind time.
- `f.toString()`, `f as string`, `"" + f`, `io.print(f)`, and container
  formatting render `"func <name>"` (static handles — including bound
  non-virtual method references), `"method <name>"` (virtual-dispatch
  handles), or `"lambda"` (closures).
- `List<func<...>>`'s `contains` / `indexOf` compare elements by
  `==` equality — two independent references to the same function
  and instantiation count as a match (§12.1).

### Restrictions

*Informative:* each of the following is a compile-time error:

- Referencing a function/method whose signature does not exactly match
  the expected `func` type.
- Referencing functions or methods with **default parameters** —
  defaults are filled only on the direct-call path.
- **Static method references** — a static method binds as a function
  value through the type name (`Util.twice`), including inherited
  statics, interface statics, native statics, and the unqualified form inside
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
- **The `out` type marker anywhere in a type argument** (`func<out int,
  ...>`, `List<out int>`) — rejected with the compile error "the out
  type marker is retired; use the reference mark instead.".
- **`func<...>(...)`** — constructing from a name is not supported;
  bind a reference instead.
- **`Dict<func<...>, V>`** — `func` as a Dict key (see the identity note
  above).
- **Boxing into `Object`** (`Object o = f`; see
  [Object and Boxing](../04-types/object.md)) — via the generic
  incompatible-type diagnostic.
- **`void` type arguments outside `func`** (`List<void>`).
- Function values as `switch` discriminants (no case family matches).
- Function values cannot cross the serialization methods — `writeStruct`
  / `writeObject` on a struct/class holding a `func` field is a run-time
  error (chapter 14).
- **A lambda with no expected function type** — a lambda literal in no
  consumer position.
- **A lambda parameter-list entry that is not a parameter shape**
  (`invalid lambda parameter list` — a bare expression mixed into the
  parameter list).
- **A lambda parameter with a default value** — the grammar has no
  default slot (`a lambda parameter cannot have a default value`).
- **An expression body producing a value under a `func<void>`
  expectation**; a block body with no `return` under a non-`void`
  expectation.
- **A lambda's explicit parameter types disagreeing with the
  expectation** — exact match position by position, no variance.
- **A lambda with a generic parameter list** (`<T>` parameters).
- **A reference to an undeclared name inside a lambda body** — the
  same name-resolution rules as an ordinary function body.
- **A lambda as an instance field initializer** — instance field
  initializers are constant-expression positions. A static field
  initializer may hold a lambda; the class's static initializer
  evaluates it once at first use.
- **A lambda in a static context referencing `this`** (§6.1).
