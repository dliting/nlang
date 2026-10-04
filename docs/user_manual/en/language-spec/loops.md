# Loops


NLang has three loop forms: `for`, `while`, and `do-while`. They share the
`bool` condition rule and `break`/`continue` semantics (see
[Statements](statements.md) "Condition typing").

### `for`

```nlang
for (init; cond; fini) { body }
```

Three-clause semantics:

- **The init clause runs once** (before the first condition evaluation);
- **The condition clause is evaluated before each iteration** (including the
  first); the body runs only when it is `true`;
- **The step clause runs after every body execution** (including the final
  iteration before the loop ends naturally).

`break` exits the loop immediately (**skipping the step clause**); `continue`
jumps to the next iteration (**the step clause runs first**, then the condition
is re-evaluated). Variables declared in the init clause are **function-scoped**
— like `foreach` loop variables, neither is confined by block scope (see
[Foreach](foreach.md)); other declarations inside the body are visible only
within it (block scope).

```nlang
int sum = 0;
for (int i = 1; i <= 10; i = i + 1) {
    sum = sum + i;
}
// sum == 55; `i` is still in scope after the loop (function-scoped)
```

```nlang
int count = 0;
for (int j = 0; j < 20; j = j + 1) {
    if (j == 5) { break; }        // exits the loop (step clause skipped)
    if (j % 2 == 1) { continue; } // step clause runs, then next iteration
    count = count + 1;
}
// count == 3 (iterations j = 0, 2, 4 counted; j = 5 hits break)
```

### `while`

```nlang
while (cond) { body }
```

`while` **tests first, then executes** — if the condition is `false` from the
start, the body may never run. `break` exits the loop; `continue` jumps to the next
iteration (back to the condition test); variables declared inside the body are
visible only within it (block scope).

```nlang
int steps = 0;
while (steps < 3) {
    steps = steps + 1;
}
// steps == 3
```

### `do-while`

```nlang
do { body } while (cond);
```

`do-while` **executes the body first, then tests** — the body runs at least once
even when the condition is always `false`. `break` and `continue` behave as in
`while`.

```nlang
int n = 0;
do {
    n = n + 1;
} while (false);
// n == 1 — the body ran once (do-while runs at least once)
```
