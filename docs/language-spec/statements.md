# Statements


### Control Flow

```
if (cond) { ... }
if (cond) { ... } else { ... }

while (cond) { ... }
do { ... } while (cond);
for (init; cond; fini) { ... }
foreach (Type var in iterable) { ... }

break;
continue;
return;
return expr;
```

**Condition typing:** `if`/`while`/`do-while`/`for`/`assert` conditions
must be `int` (comparisons produce `int`). String, float, class, struct,
and array conditions are compile errors — the VM's `OP_JumpIfNot` reads
a single int32, and non-int values (string pool handles, heap indices)
have no meaningful truthiness. Use an explicit comparison instead:
`if (s != "")`, `if (obj != null)`.

### Foreach Statement (Phase 8e-5)

```
foreach (Type var in iterable) { body }
```

Iterates the elements of `iterable`, binding each to `var` for the body.
Supported iterables:

| Iterable | Iterates | Element access |
|----------|----------|----------------|
| `T[N]` (array) | elements `arr[0]..arr[N-1]` | `OP_LoadElement` |
| `List<T>` | elements in insertion order | `List<T>.get(i)` |
| `Dict<K,V>` | **keys** (Python style) | inline `dict.keys()` then `List<K>.get(i)` |

`break` and `continue` work identically to `for`. The loop variable is
**function-scoped** (NLang has no block scope, consistent with `for`):

```
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// `x` remains in scope here (function-scoped)
```

**Dict iteration example**:

```
Dict<string, int> ages = new Dict<string, int>();
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**Mutation is undefined behavior**. The element count is cached at loop
entry (`n = iterable.length()` for List/Dict, `n = arr.length` for Array).
Structural modifications inside the body (`List.Add`/`RemoveAt`,
`Dict.Set`/`Remove`) may cause: out-of-bounds access, skipped/duplicated
elements, or stale `Keys()` snapshots. Element assignment (`arr[i] = x`)
inside an Array foreach body is fine (no structural change).

**Struct elements are copied into the loop variable** (value semantics):
`foreach (Point p in arr) { p.x = 99; }` does not modify `arr`'s elements —
`p` is a fresh deep copy per iteration (consistent with C#, where foreach
over value-type elements also yields copies).

**Null iterable** throws NPE on the first `length()`/`Length()` call
(consistent with all other class-typed calls).

**`List<int>` with value 0**: due to a pre-existing `OP_Box` optimization
(literal `0` is treated as the null sentinel), `foreach` over a `List<int>`
containing literal-zero elements currently throws `unbox on null/invalid
reference`. This is a boxing limitation, not a `foreach` bug — work around
by avoiding 0 as a list element value. A future phase will revisit the
null-sentinel design.

### Compound Assignment (Phase 9a)

```
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values:
local variables, class fields (`this.f += y`), struct fields (`pt.x += y`).
The left value is evaluated **only once** (so `obj.something() += 1` would
not double-invoke `something()`).

Not supported: subscript left-value (`arr[i] += 1`). The bytecode frame
layout doesn't have enough scratch slots for single-evaluation of
subscript read-modify-write. Use the explicit form `arr[i] = arr[i] + 1`.

### Assert Statement (Phase 9a)

```
assert(condition);
```

Evaluates `condition`. If false, throws an `AssertionException` which can be
caught by a `try/catch` block (Phase 9d). If uncaught, terminates the program
with exit code 1. Single-argument form only (no message override yet).

### Exception Handling (Phase 9d)

NLang supports structured exception handling with a Java/C#-style class
hierarchy. All exceptions are instances of `Exception` or its subclasses.

**Built-in exception classes:**

| Class | Superclass | Thrown by |
|-------|-----------|-----------|
| `Exception` | `Object` | User `throw` / Dict key not found |
| `NullPointerException` | `Exception` | Null reference access |
| `DivByZeroException` | `Exception` | Integer division/modulo by zero |
| `IndexOutOfBoundsException` | `Exception` | Array/List index out of bounds |
| `AssertionException` | `Exception` | `assert(false)` |

**try/catch:**

```
try {
    // code that may throw
} catch (DivByZeroException e) {
    // handle division by zero
} catch (Exception e) {
    // handle any other exception
}
```

- Multiple `catch` clauses are supported, matched in declaration order.
- The first matching catch clause executes; subsequent ones are skipped.
- `catch (Exception e)` catches all exceptions (Exception is the base class).
- The catch variable `e` is a regular local variable within the catch body.

**throw:**

```
throw new Exception("error message");   // throw a new exception
throw;                                   // re-throw current exception (only inside catch)
```

- `throw expr` — the expression must evaluate to an Exception subclass instance.
  Throwing a non-Exception value (e.g. `throw 42`) is a compile error.
- `throw;` (re-throw) is only valid lexically inside a `catch` body. Using it
  outside a catch block is a compile error.

**User-defined exception subclasses:**

```
class MyException : Exception {
    int code;
    public int MyException(string msg) {
        super(msg);          // forward to Exception(message) ctor
        this.code = 42;
        return 0;
    }
}
```

User classes can extend `Exception` to carry additional fields. Without a
user constructor, the default constructor is used and inherited fields are
zero-initialized; a user ctor typically forwards the message via
`super(msg)` (see "super() — constructor chaining" below).

**Exception fields:**

Exception instances expose two readable/writable fields:

- `message` (string) — the exception message. Set by the constructor
  (`new Exception("msg")`) or by VM error sites. Writable by user code.
- `backtrace` (List&lt;string&gt;) — call stack snapshot at throw time for
  VM-thrown exceptions (`funcName.n:line` entries, innermost first).
  User-constructed exceptions start with an empty backtrace.

```
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();       // > 0 — VM sets the message
    int frames = e.backtrace.length(); // >= 1 — VM snapshots the stack
}

//User subclasses inherit both fields; own fields land after them.
class MyException : Exception {
    int code;
}
MyException e = new MyException();
e.message = "custom";   // writable
e.code = 42;

**VM errors are catchable:**

Runtime errors that previously caused hard crashes (NPE, division by zero,
array/list index out of bounds, assertion failure) now throw the corresponding
Exception subclass and can be caught:

```
try {
    int x = 0;
    int y = 1 / x;           // throws DivByZeroException
} catch (DivByZeroException e) {
    // caught
}

try {
    List<int> lst = new List<int>();
    return lst.get(999);      // throws IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // caught
}
```

**Uncaught exceptions** propagate up the call stack. If no handler is found,
the program terminates with exit code 1 (same as the pre-9d behavior).

**finally (Phase 9d-2):**

```
try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // always runs
}
```

`finally` has full Java semantics — the finally body runs when control
leaves the try region by **any** of these paths:

- try body completes normally (including falling out the bottom)
- a catch clause completes (matched or not)
- an exception unwinds through (the finally handler runs its body copy,
  then re-throws the original exception; this includes exceptions thrown
  from inside a catch body)
- `break` / `continue` transfer out of the region (an inline copy of the
  finally body runs before the jump, innermost-first for nested tries)
- `return` (the return expression is evaluated **first**, then the finally
  body runs, then the function returns)

Nesting: inner finally bodies run before outer ones; after them, the
surrounding catch (if any) sees the exception. Each finally body executes
exactly once per control-flow pass — the normal-path and exception-path
copies are disjoint code regions.

`try { } finally { }` without any catch clause is legal (the finally entry
is the only handler).

**Restriction**: `break`, `continue`, `return`, and `throw` are not
allowed *inside a finally body* (compile error). A finally body must not
swallow the in-flight control flow or exception.

**super() — constructor chaining (Phase 9d-2):**

```
class Base {
    public int v;
    public int Base(int x) { this.v = x; return 0; }
}
class Kid : Base {
    public int Kid(int x) {
        super(x * 2);        // calls Base(int)
        return 0;
    }
}
```

- `super(args);` invokes the **direct parent class's constructor** on the
  same `this` object. Valid only inside a constructor of a class that has
  a parent (`Object` has none).
- May appear at **any statement position** in the ctor (not restricted to
  the first statement).
- Argument count must match the parent ctor's parameter count. For parents
  in the built-in Exception family the ctor takes exactly one `message`
  argument.
- Named arguments (`super(x = 1)`) are not supported (compile error).
- `super()` with no arguments against a parent with no constructor is a
  legal no-op; passing arguments in that case is a compile error.

### Const Local Variables (Phase 9a)

```
const int X = 5;
const string Greeting = "hello";
```

Local variables marked `const` must be initialized at declaration and
cannot subsequently be assigned or compound-assigned. Only **local**
const is supported; class/struct field const is not (constructor
initialization order would add complexity, deferred to a future phase).

**Const is shallow (Java-`final`-style)**: `const` prevents rebinding the
*name* but does not freeze the referenced object's state. Member mutation
through a const local is allowed:

```
const Foo f = new Foo();
f.x = 5;            // OK — f itself is not reassigned
f = new Foo();      // ERROR — cannot reassign const local
const Point p = q;
p.x = 5;            // OK — only p's binding is const
```

For class fields and array elements this means: a `const` reference still
permits writing through it. Deep/immutability-style const is intentionally
out of scope for Phase 9a and may be revisited in a future phase.

### Switch

```
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

**Discriminant families.** The switch discriminant may be an `int`, a
`float`, a `string`, or an enum-typed value (enums compare as their int
values). Class, struct, array, and `null` discriminants are rejected at
compile time.

**Typed equality.** Comparison uses the equality operator of the
discriminant's family: ints and enums compare exactly; floats compare under
IEEE semantics (`-0.0 == 0.0` is true, NaN never equals anything, including
itself); strings compare by content, not by identity. Case labels must
belong to the same family as the discriminant — no cross-family conversion
(`case "1"` on an int discriminant is a compile error). `null` is not a
valid case label. Labels may be computed expressions (e.g. `case f(x):`) —
they are evaluated in clause order at run time.

**Multi-value labels.** A case clause may list several labels
(`case 1, 2:`); the body runs when any of them matches.

**No fall-through.** Each case body ends with an implicit jump out of the
switch — execution does not cascade into the next case body even without an
explicit `break` statement. This matches Java/C# semantics, not C/C++. The
`break` keyword is only needed to exit early from inside a multi-statement
case body. A `break` inside a case body always binds to the switch itself,
never to an enclosing loop.

**Duplicate labels.** Constant labels (numeric/string literals and enum
members) with the same value within one switch — across clauses or inside
one multi-value clause — are rejected at compile time (`case 1:` plus
`case Color.Red:` where `Red = 0` is a duplicate). Duplicate non-constant
labels (two calls that both return 1) are allowed; the first match wins.

### Null Check

Accessing a field or method on a null class reference throws a
`NullPointerException` (Phase 9d), which can be caught by a `try/catch`
block. If uncaught, the program terminates with exit code 1.
