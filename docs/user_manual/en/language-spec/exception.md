# Exceptions


NLang supports structured exception handling with a Java/C#-style class
hierarchy. All exceptions are instances of `Exception` or its subclasses.

### Built-in exception classes

| Class | Superclass | Thrown by |
|-------|-----------|-----------|
| `Exception` | `Object` | User `throw` / Dict key not found |
| `NullPointerException` | `Exception` | Null reference access |
| `DivByZeroException` | `Exception` | Integer division/modulo by zero |
| `IndexOutOfBoundsException` | `Exception` | Array/List index out of bounds |
| `AssertionException` | `Exception` | `assert(false)` |

### `try` / `catch`

```nlang
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

### `throw`

```nlang
throw new Exception("error message");   // throw a new exception
throw;                                   // re-throw current exception (only inside catch)
```

- `throw expr` — the expression must evaluate to an Exception subclass
  instance. Throwing a non-Exception value (e.g. `throw 42`) is a compile
  error.
- `throw;` (re-throw) is only valid lexically inside a `catch` body. Using it
  outside a catch block is a compile error.

### User-defined exception subclasses

```nlang
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
zero-initialized; a user ctor typically forwards the message via `super(msg)`
(see [Class](class.md) "super() — constructor chaining").

### Exception fields

Exception instances expose two readable/writable fields:

- `message` (string) — the exception message. Set by the constructor
  (`new Exception("msg")`) or by VM error sites. Writable by user code.
- `backtrace` (List&lt;string&gt;) — call stack snapshot at throw time for
  VM-thrown exceptions (`funcName.n:line` entries, innermost first).
  User-constructed exceptions start with an empty backtrace.

```nlang
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
```

### VM errors are catchable

Runtime errors (NPE, division by zero, array/list index out of bounds,
assertion failure) throw the corresponding Exception subclass and can be
caught by `try/catch`:

```nlang
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
the program terminates with exit code 1.

### `finally`

```nlang
try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // always runs
}
```

`finally` has full Java semantics — the finally body runs when control leaves
the try region by **any** of these paths:

- try body completes normally (including falling out the bottom)
- a catch clause completes (matched or not)
- an exception unwinds through (the finally handler runs its body copy, then
  re-throws the original exception; this includes exceptions thrown from
  inside a catch body)
- `break` / `continue` transfer out of the region (an inline copy of the
  finally body runs before the jump, innermost-first for nested tries)
- `return` (the return expression is evaluated **first**, then the finally body
  runs, then the function returns)

Nesting: inner finally bodies run before outer ones; after them, the
surrounding catch (if any) sees the exception. Each finally body executes
exactly once per control-flow pass — the normal-path and exception-path copies
are disjoint code regions.

`try { } finally { }` without any catch clause is legal (the finally entry is
the only handler).

**Restriction**: `break`, `continue`, `return`, and `throw` are not allowed
*inside a finally body* (compile error). A finally body must not swallow the
in-flight control flow or exception.

### Debugging exceptions

Press F5 in nide to start a debug session; turn on the Break on exceptions
toggle to pause at the throw point (see
[Debugging in nide](../getting-started/debugging.md)); on the command line,
`ndb`'s `catch on` breaks at the throw point as well (see
[command-line ndb](../cli-tools/ndb.md)).
