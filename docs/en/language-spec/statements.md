# Statements


NLang's control-flow statements. This page covers the rules they share; each
specific statement has its own page.

### Statement pages

| Statement                                  | Page |
|--------------------------------------------|------|
| `if` / `else`                              | [If](if.md) |
| `for` / `while` / `do-while`               | [Loops](loops.md) |
| `foreach`                                  | [Foreach](foreach.md) |
| `switch` / `case` / `default`              | [Switch](switch.md) |
| `try` / `catch` / `throw` / `finally`      | [Exceptions](exception.md) |
| `assert`                                   | [Assert](assert.md) |
| compound assignment (`+=` `-=` `*=` `/=` `%=`) | [Compound Assignment](compound-assignment.md) |
| `const` local variable                     | [Const Local](const-local.md) |

### Condition typing

`if`/`while`/`do-while`/`for`/`assert` conditions must be `int` (comparisons
produce `int`). String, float, class, struct, and array conditions are
compile errors — the VM's `OP_JumpIfNot` reads a single int32, and non-int
values (string object handles, heap indices) have no meaningful truthiness.
Use an explicit comparison instead: `if (s != "")`, `if (obj != null)`.

### `break`, `continue`, `return`

- `break;` — exit the enclosing loop (or `switch`), skipping any step clause.
- `continue;` — jump to the next loop iteration (the step clause runs first in
  a `for`).
- `return;` / `return expr;` — exit the enclosing function.

Each statement page repeats these where the construct is defined.

### Unbraced body

A control-flow statement's unbraced (bare) body cannot be a variable
declaration — `if (c) int x = 1;` reports `a local declaration cannot be the
unbraced body of a control-flow statement`. Use a braced body instead.
