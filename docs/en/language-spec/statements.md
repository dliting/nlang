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

`if`/`while`/`do-while`/`for`/`assert` conditions must be **`bool`** —
comparisons and predicates already produce bool (see
[Operators](operators.md), [Primitives](primitives.md)). int, string,
float, char, class, struct, and array conditions are compile errors
(`if condition must be bool, not "Int32"`). NLang is a strict-bool
language with no C-style "non-zero is true": `if (1)` and `if (count)`
are both illegal — write `if (count != 0)`; likewise `if (s != "")`,
`if (obj != null)`.

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
