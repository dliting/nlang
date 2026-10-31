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
| `InvalidCastException` | `Exception` | A failed `as` downcast (the actual type is not on the target's super-chain) |
| `IOException` | `Exception` | File I/O failure |
| `ChannelClosedException` | `Exception` | `send`/`recv` on a closed channel; declared by the `concurrent` package, so it arrives with `import concurrent;` (see [Channels](concurrency-channel.md)) |

Every built-in exception class is a const class (see [Class](class.md) "const classes and const structs"),
and every constructor has the same shape: `XxxException(string message,
Exception cause = null)` — the second argument is optional and wraps the
causing exception (see "Exception fields" below).

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
throw Exception("error message");   // throw a new exception
throw;                                   // re-throw current exception (only inside catch)
```

- `throw expr` — the expression must evaluate to an Exception subclass
  instance. Throwing a non-Exception value (e.g. `throw 42`) is a compile
  error.
- `throw;` (re-throw) is only valid lexically inside a `catch` body. Using it
  outside a catch block is a compile error.

### User-defined exception subclasses

```nlang
const class MyException : Exception {
    int code;
    public MyException(string msg, int c) {
        super(msg);          // forward to Exception(message, cause = null)
        this.code = c;       // own fields are assigned in the ctor
    }
}
```

User classes can extend `Exception` to carry additional fields. The
Exception family is const, so user subclasses must be declared `const
class` — a const class can only be extended by another const class (see
[Class](class.md) "const classes and const structs"). Without a user
constructor, the default constructor is used and inherited fields are
zero-initialized. Extra fields may only be assigned inside the subclass's
own constructor; inherited base fields cannot be rewritten there — pass
supplements forward through `super(msg)` or `super(msg, cause)` (see
[Class](class.md) "super() — constructor chaining").

### Exception fields

Exception instances expose three read-only fields — exception classes are
const, so the field bindings cannot be written after construction; a write
outside a constructor is a compile error (`"message" is immutable`):

- `message` (string) — the exception message. Set by the constructor
  (`Exception("msg")`) or by VM error sites.
- `cause` (Exception) — the wrapped causing exception, passed through the
  second constructor argument; null when nothing is wrapped. Each link
  points to the previous one at construction time only, so the chain can
  never form a cycle; there is no `initCause`-style after-the-fact setter.
- `backtrace` (List&lt;string&gt;) — call stack snapshot at throw time for
  VM-thrown exceptions (`funcName.n:line` entries, innermost first).
  User-constructed exceptions start with an empty backtrace.

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();        // > 0 — VM sets the message
    int frames = e.backtrace.length(); // >= 1 — VM snapshots the stack
}

//Cause chain: wrap the underlying failure in a higher-level exception.
Exception root = Exception("disk full");
Exception wrap = Exception("save failed", root);
if (wrap.cause != null) {
    string rootMsg = wrap.cause.message;   // "disk full"
}
```

Subclasses inherit all three fields; own fields (like `code`) land after
`cause` and may only be assigned in the subclass constructor. Reading a
`List`/`Dict` field (such as `backtrace`) returns a **copy** (the const
container-field read rule, see [Class](class.md) "const classes and const
structs") — `add` or subscript writes on the copy never touch the
exception itself; two reads yield distinct copies, so comparing them with
`==` is meaningless.

### VM errors are catchable

Runtime errors — null pointer exception (NPE), division by zero,
array/list index out of bounds, assertion failure — throw the
corresponding Exception subclass and can be caught by `try/catch`:

```nlang
try {
    int x = 0;
    int y = 1 / x;           // throws DivByZeroException
} catch (DivByZeroException e) {
    // caught
}

try {
    List<int> lst = {};
    return lst.get(999);      // throws IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // caught
}
```

A failed `as` class downcast likewise raises `InvalidCastException` —
catchable with `catch`, its `message` carrying the expected/actual
class leaf names (see [Type Casts](type-casts.md)).

**Uncaught exceptions** propagate up the call stack. If no handler is found,
the program terminates with exit code 1, and the error report walks the
cause chain one link per line:

```
Runtime error: Exception: save failed
Caused by: Exception: disk full
```

An unobserved failure in a spawned task is reported the same way at run end
(a `failed with uncaught ... (never joined)` line followed by `Caused by:`
lines; see [Threads](concurrency-thread.md)).

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
- an exception propagates through (the finally body runs, then the
  original exception is re-thrown; this includes exceptions thrown from
  inside a catch body)
- `break` / `continue` transfer out of the region (the finally body runs
  before the jump, innermost-first for nested tries)
- `return` (the return expression is evaluated **first**, then the finally body
  runs, then the function returns)

Nesting: inner finally bodies run before outer ones; after them, the
surrounding catch (if any) sees the exception. Each finally body executes
exactly once per control-flow pass.

`try { } finally { }` without any catch clause is legal (the finally
clause is the only handler).

**Restriction**: `break`, `continue`, `return`, and `throw` are not allowed
*inside a finally body* (compile error). A finally body must not swallow the
in-flight control flow or exception.

### Debugging exceptions

Press F5 in nide to start a debug session; turn on the Break on exceptions
toggle to pause at the throw point (see
[Debugging in nide](../getting-started/debugging.md)); on the command line,
`ndb`'s `catch on` breaks at the throw point as well (see
[command-line ndb](../cli-tools/ndb.md)).
