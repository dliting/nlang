# Assert


```nlang
assert(condition);
```

Evaluates `condition`. If false, throws an `AssertionException` which can be
caught by a `try/catch` block. If uncaught, terminates the program with exit
code 1. Single-argument form only (custom failure messages are not
supported). The condition must be `bool` (see [Statements](statements.md)
"Condition typing").

```nlang
int x = 5;
assert(x > 3);            // passes — nothing happens
try {
    assert(x > 10);        // throws AssertionException
} catch (AssertionException e) {
    // caught
}
```

See [Exceptions](exception.md) for the `AssertionException` class and the
`try/catch` mechanism.
