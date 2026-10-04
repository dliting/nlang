# Switch


```nlang
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

The discriminant must be `int`, `float`, `string`, or an enum-typed value
(enums compare as their int values). Class, struct, array, and `null`
discriminants are rejected at compile time — see
[Statements](statements.md) "Condition typing" for the shared `int` rule and
[Type Semantics](type-semantics.md) for what each type can be.

**Typed equality.** Comparison uses the equality operator of the discriminant's
family: ints and enums compare exactly; floats compare under IEEE semantics
(`-0.0 == 0.0` is true, NaN never equals anything, including itself);
strings compare by content, not by identity. Case labels must belong to the
same family as the discriminant — no cross-family conversion (`case "1"` on an
int discriminant is a compile error). `null` is not a valid case label. Labels
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
