# 8. Exceptions

Exception handling is structured: a raised exception is an
ordinary object, an enclosing `try` statement may handle it, and
an unhandled exception ends the program. The syntax of `throw`
statements and `try` statements is §2.5.

## 8.1 Throw and Propagation

`throw e;` evaluates `e`. The value must be an instance of
`Exception` or of a subclass of it (§8.3, §8.4); any other type is
a compile-time error. A thrown exception is an ordinary class
instance (§4.8), so the throwing form is a constructor call:
`throw Exception("message");`. `throw` takes a value, not a class
name.

`throw;` re-throws the exception currently being handled. It is
valid only lexically inside the body of a `catch` clause; anywhere
else it is a compile-time error. A re-throw propagates the same
exception object: its class, its `message` and its `backtrace` are
unchanged (§8.3).

A throw transfers control out of the running function to the
nearest enclosing `try` statement with a matching `catch` clause,
in the caller and further up the call chain (§8.2); the `finally`
bodies along the path run as §8.2 defines. If no handler matches,
the host reports the uncaught exception and the program terminates
with exit code 1 (chapter 9). The report walks the cause chain one
link per line: a top line naming the exception's class and
message, followed by one `Caused by:` line per cause link (§8.3).

```
Runtime error: Exception: save failed
Caused by: Exception: disk full
```

An unobserved failure in a spawned task is reported the same way
at run end (§10.9).

The language, the standard library and the VM raise the built-in
exceptions of §8.3 for the errors they report, and those are
catchable: access through a `null` reference (§4.8, §5.6, §5.7),
division or remainder by zero and an out-of-range shift count
(§5.2), an out-of-range subscript (§5.7), a failed assertion
(§6.5), a failed `as` downcast (§5.5), and the library errors of
chapter 19.

A *host-level* run-time error (§7.1) is not an exception: no
`catch` clause handles it and the program ends. The sections that
define host-level failures name their sites — the call-depth
bound (§7.1), a missing native function (§7.5), the unboxing and
`null`-downcast failures of §5.5, an enumeration `toString()`
beyond its member table (§4.6), the `toString` nesting bound of
§11.2, the stream failures of §14.5, and the concurrency
failures of chapter 10.

```nlang
throw Exception("error message");   // throw a new exception
throw;                              // re-throw current exception (only inside catch)
```

*Informative:* the expression form is checked at compile time —
`throw 42` is a compile error. Run-time errors raised by the VM are
ordinary exceptions and are caught by `try`/`catch`:

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

A failed `as` downcast likewise raises `InvalidCastException`,
catchable like any other exception; its `message` carries the
expected and actual class names (§4.5).

## 8.2 try / catch / finally

The grammar (§2.5) permits an empty catch list, but a `try`
statement with neither a `catch` clause nor a `finally` clause is
a compile-time error; `try { } finally { }` with no `catch` is
legal.

The type of a `catch` clause must be `Exception` or a subclass of
it; any other type is a compile-time error. The clauses of a `try`
statement are matched in declaration order. The first clause whose
type is the raised exception's class or an ancestor of that class
runs its body; the remaining clauses are skipped.
`catch (Exception e)` therefore matches every exception, and a
clause for a more specific class listed after it is unreachable.
The `catch` parameter is a regular local variable of the `catch`
body (§3.5): it may be read and assigned.

The `finally` body runs whenever control leaves the `try` region,
by every path:

- the `try` body completes normally;
- a matched `catch` body completes;
- an exception unwinds through: the `finally` body runs and the
  exception continues to propagate — including an exception thrown
  from a `catch` body;
- a `break` or `continue` transfers out of the region: the
  `finally` body runs before the jump, innermost first for nested
  `try` statements;
- a `return`: the return expression is evaluated first, then the
  `finally` body runs, then the function returns.

Each `finally` body runs exactly once per control-flow pass.
Nested `finally` bodies run innermost first; after they run, an
enclosing `catch` sees the in-flight exception.

Inside a `finally` body, `break`, `continue`, `return` and `throw`
are compile-time errors: the body must not displace the control
flow in flight.

*Informative:* the statement shapes follow the Java/C# style familiar
from other languages:

```nlang
try {
    // code that may throw
} catch (DivByZeroException e) {
    // handle division by zero
} catch (Exception e) {
    // handle any other exception
}

try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // always runs
}
```

## 8.3 Built-in Exceptions

`Exception` extends `Object` (§4.11). The built-in exception
classes are `Exception` and its seven direct subclasses:

| Class | Raised by |
| --- | --- |
| `Exception` | a user `throw` of `Exception` itself; a `Dict` read for an absent key (§4.12); VM sites with no class of their own: an out-of-range shift count (§5.2), a `char` conversion outside the Unicode scalar values (§5.5), a `Channel` constructed with a negative capacity (§10.4) |
| `NullPointerException` | a member access, method call or call through a `null` class, interface or function reference (§4.8, §5.6, §5.7) |
| `DivByZeroException` | division or remainder by zero, at any numeric width (§5.2) |
| `IndexOutOfBoundsException` | an array or `List` subscript outside its bounds (§5.7); a sized array creation with a negative length or an element count past the bound (§4.10); a `string` `substring` or `charAt` out of range (§16.2) |
| `AssertionException` | a failed `assert` (§6.5) |
| `InvalidCastException` | a failed `as` downcast (§5.5) |
| `IOException` | a failed `io` or `fs` operation (chapter 14, chapter 15) |
| `InitializationException` | a static initializer failing through the initialization protocol, wrapping the thrown object as its cause (§4.8.8); a read of an initializer-bearing static — class, interface or static local — from inside the initialization it awaits (§4.8.8, §6.1) |

The `concurrent` package (§3.9.4) declares one further direct
subclass, `ChannelClosedException`, raised by operations on a
closed channel (§10.4); it reaches scope through
`import concurrent;` like any package class, and `catch` treats
it like every other subclass.

Every built-in exception class is a const class (§4.8.12), and
every constructor has the same shape:
`XxxException(string message, Exception cause = null)` — the
second argument is optional and wraps the causing exception.

Every exception instance carries three read-only fields,
inherited from `Exception`; the classes are const, so a write
outside a constructor is a compile-time error (§4.8.12):

- `message` (`string`) — set by the constructor call that creates
  the instance, or by the site that raises a VM exception. Where a
  section fixes the text (§6.5), that text is normative;
  otherwise the message describes the error in words chosen by the
  implementation. User code may read the field.
- `cause` (`Exception`) — the wrapped causing exception, passed
  through the second constructor argument; `null` when nothing is
  wrapped. Each link points to the previous one at construction
  time only, so the chain can never form a cycle; there is no
  after-the-fact setter.
- `backtrace` (`List<string>`, §4.12) — for an exception raised by
  the VM, a snapshot of the call stack taken at the raise: one
  entry per frame, innermost first, each entry
  `funcName (file:line)`. A free function is named by its
  module-qualified name; a method is named by its bare method
  name. `file` is the source path recorded when the module was
  compiled; a frame whose line is unknown renders `?`, and a
  module with no recorded source path renders its module name with
  a `.n` suffix. An exception created by user code starts with an
  empty backtrace. Reading the field returns a copy (§4.8.12):
  `add` or subscript writes on the copy never touch the exception,
  and two reads yield distinct copies.

*Informative:* the fields are read-only outside the constructors:

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();        // > 0 — VM sets the message
    int frames = e.backtrace.length(); // >= 1 — VM snapshots the stack
}

// Cause chain: wrap the underlying failure in a higher-level exception.
Exception root = Exception("disk full");
Exception wrap = Exception("save failed", root);
if (wrap.cause != null) {
    string rootMsg = wrap.cause.message;   // "disk full"
}
```

## 8.4 User Exception Classes

A user class may extend `Exception` (§4.8.5) to carry additional
state. The Exception family is const, so a user exception class
must be declared `const class`: a const class can only be
extended by another const class (§4.8.12). Its constructor
forwards the message with `super(msg)` or `super(msg, cause)`
(§4.8.6); with no declared constructor, the synthesized
zero-argument constructor leaves the inherited fields at their
defaults (§4.8.3, §4.8.6). The class's own fields follow the
inherited ones in the object layout (§4.8.5) and may be assigned
only inside the subclass's own constructor (§4.8.12).

A user exception is raised and handled exactly like a built-in
one: `catch (MyException e)` matches `MyException` and its
subclasses under the rule of §8.2, and `catch (Exception e)`
matches it as well.

```nlang
const class MyException : Exception {
    int code;
    public MyException(string msg, int c) {
        super(msg);          // forward to Exception(message, cause = null)
        this.code = c;       // own fields are assigned in the ctor
    }
}
```

### Debugging exceptions

*Informative:* press F5 in nide to start a debug session and turn on
the Break on exceptions toggle to pause at the throw point (see
[Debugging in nide](../../getting-started/debugging.md)); on the
command line, `ndb`'s `catch on` breaks at the throw point as well
(see [command-line ndb](../../cli-tools/ndb.md)).
