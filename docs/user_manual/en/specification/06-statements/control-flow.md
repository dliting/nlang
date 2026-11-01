# 6.7 Control Flow

The normative rules for `if`, `switch` and `assert` are §6.3 and §6.5.

### If

```nlang
if (cond) { body }
if (cond) { body } else { body }
```

The first body runs when the condition is `true`; otherwise the `else` body
runs (if present). The condition must be `bool` (see
[Statements](index.md) "Condition typing"). Another `if` may follow `else` to
form chained tests — the `else` body is simply another `if` statement; there
is no standalone `else if` keyword (the one-word `elseif` form is a syntax
error).

The body is a compound statement: variables declared inside it are visible only
within that body (block scope).

```nlang
int n = 7;
if (n > 10) {
    n = 1;
} else if (n > 5) {
    n = 2;
} else {
    n = 3;
}
// n == 2
```

### Switch

```nlang
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

The discriminant must be an integer type, a floating-point type, `char`,
`string`, or an enum-typed value (enums compare as their int values).
Class, struct, array, `bool`, and `null` discriminants are rejected at
compile time — see [Types](../04-types/index.md) for what each type
can be.

**Typed equality.** Comparison uses the equality operator of the discriminant's
family: ints and enums compare exactly; floats compare under IEEE semantics
(`-0.0 == 0.0` is true, NaN never equals anything, including itself);
strings compare by content, not by identity. Case labels must belong to the
same family as the discriminant — no cross-family conversion (`case "1"` on an
int discriminant is a compile error) — and constant labels must fit the
discriminant's own range (`case 300` on a `byte` discriminant is a compile
error). `null` is not a valid case label. Labels
may be computed expressions (e.g. `case f(x):`) — they are evaluated in clause
order at run time.

**Multi-value labels.** A case clause may list several labels (`case 1, 2:`);
the body runs when any of them matches.

**No fall-through.** Each case body ends with an implicit jump out of the
switch — execution does not cascade into the next case body even without an
explicit `break` statement. This matches Java/C# semantics, not C/C++. The
`break` keyword is only needed to exit early from inside a multi-statement
case body. A `break` inside a case body always binds to the switch itself,
never to an enclosing loop.

**Duplicate labels.** Constant labels (numeric/string literals and enum
members) with the same value within one switch — across clauses or inside one
multi-value clause — are rejected at compile time (`case 1:` plus
`case Color.Red:` where `Red = 0` is a duplicate). Duplicate non-constant
labels (two calls that both return 1) are allowed; the first match wins.

### Assert

```nlang
assert(condition);
```

Evaluates `condition`. If false, throws an `AssertionException` which can be
caught by a `try/catch` block. If uncaught, terminates the program with exit
code 1. Single-argument form only (custom failure messages are not
supported). The condition must be `bool` (see
[Statements](index.md) "Condition typing").

```nlang
int x = 5;
assert(x > 3);            // passes — nothing happens
try {
    assert(x > 10);        // throws AssertionException
} catch (AssertionException e) {
    // caught
}
```

See [Exceptions](../../language-spec/exception.md) for the `AssertionException` class and the
`try/catch` mechanism.
