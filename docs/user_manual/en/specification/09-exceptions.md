# 9. Exceptions

Exception handling is structured: a raised exception is an
ordinary object, an enclosing `try` statement may handle it, and
an unhandled exception ends the program. The syntax of `throw`
statements and `try` statements is §2.5.

## 9.1 Throw and Propagation

`throw e;` evaluates `e`. The value must be an instance of
`Exception` or of a subclass of it (§9.3, §9.4); any other type is
a compile-time error. A thrown exception is an ordinary class
instance (§4.7), so the throwing form is a constructor call:
`throw Exception("message");`. `throw` takes a value, not a class
name.

`throw;` re-throws the exception currently being handled. It is
valid only lexically inside the body of a `catch` clause; anywhere
else it is a compile-time error. A re-throw propagates the same
exception object: its class, its `message` and its `backtrace` are
unchanged (§9.3).

A throw transfers control out of the running function to the
nearest enclosing `try` statement with a matching `catch` clause,
in the caller and further up the call chain (§9.2); the `finally`
bodies along the path run as §9.2 defines. If no handler matches,
the host reports the uncaught exception and the program terminates
with exit code 1 (chapter 10).

The language, the standard library and the VM raise the built-in
exceptions of §9.3 for the errors they report, and those are
catchable: access through a `null` reference (§4.7, §5.6, §5.7),
division or remainder by zero and an out-of-range shift count
(§5.2), an out-of-range subscript (§5.7), a failed assertion
(§6.5), a failed `as` downcast (§5.5), and the library errors of
chapter 19.

A *host-level* run-time error (§7.1) is not an exception: no
`catch` clause handles it and the program ends. The sections that
define host-level failures name their sites — the call-depth
bound (§7.1), a missing native function (§7.5), the unboxing and
`null`-downcast failures of §5.5, an enumeration `toString()`
beyond its member table (§4.5), the `toString` nesting bound of
§13.2, the stream failures of §15.5, and the concurrency
failures of chapter 11.

## 9.2 try / catch / finally

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
The `catch`
parameter is a regular local variable of the `catch` body (§3.5):
it may be read and assigned.

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

## 9.3 Built-in Exceptions

`Exception` extends `Object` (§4.9). The built-in exception
classes are `Exception` and its six direct subclasses:

| Class | Raised by |
| --- | --- |
| `Exception` | a user `throw` of `Exception` itself; a `Dict` read for an absent key (§4.10); VM sites with no class of their own: an out-of-range shift count (§5.2), a `char` conversion outside the Unicode scalar values (§5.5), a `Channel` constructed with a negative capacity (§11.4) |
| `NullPointerException` | a member access, method call or call through a `null` class, interface or function reference (§4.7, §5.6, §5.7) |
| `DivByZeroException` | division or remainder by zero, at any numeric width (§5.2) |
| `IndexOutOfBoundsException` | an array or `List` subscript outside its bounds (§5.7); a sized array creation with a negative length or an element count past the bound (§4.3); a `string` `substring` or `charAt` out of range (§13.3) |
| `AssertionException` | a failed `assert` (§6.5) |
| `InvalidCastException` | a failed `as` downcast (§5.5) |
| `IOException` | a failed `io` or `fs` operation (chapter 15, chapter 16) |

The `concurrent` package (§12.4) declares one further direct
subclass, `ChannelClosedException`, raised by operations on a
closed channel (§11.4); it reaches scope through
`import concurrent;` like any package class, and `catch` treats
it like every other subclass.

Every exception instance carries two fields, inherited from
`Exception`:

- `message` (`string`) — set by the constructor call that creates
  the instance, or by the site that raises a VM exception. Where a
  section fixes the text (§6.5), that text is normative;
  otherwise the message describes the error in words chosen by the
  implementation. User code may read and assign the field.
- `backtrace` (`List<string>`, §4.10) — for an exception raised by
  the VM, a snapshot of the call stack taken at the raise: one
  entry per frame, innermost first, each entry
  `qualifiedFuncName (file:line)`. An exception created by user
  code starts with an empty backtrace. User code may read and
  assign the field.

## 9.4 User Exception Classes

A user class may extend `Exception` (§8.1) to carry additional
state. Its constructor forwards the message with `super(msg)`
(§8.1); with no declared constructor, the synthesized
zero-argument constructor leaves the inherited fields at their
defaults (§8.1). The class's own fields follow the inherited ones
in the object layout (§8.1).

A user exception is raised and handled exactly like a built-in
one: `catch (MyException e)` matches `MyException` and its
subclasses under the rule of §9.2, and `catch (Exception e)`
matches it as well.
