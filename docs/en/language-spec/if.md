# If


```nlang
if (cond) { body }
if (cond) { body } else { body }
```

The first body runs when the condition is non-zero; otherwise the `else` body
runs (if present). The condition must be `int` (see [Statements](statements.md)
"Condition typing"). Another `if` may follow `else` to form chained tests —
the `else` body is simply another `if` statement; there is no standalone
`else if` keyword (the one-word `elseif` form is a syntax error).

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
