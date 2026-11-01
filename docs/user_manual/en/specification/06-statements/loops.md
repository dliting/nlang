# 6.8 Loops

The normative rules are §6.4.

NLang has four loop forms: `for`, `while`, `do-while` and `foreach`. They
share the `bool` condition rule and `break`/`continue` semantics (see
[Statements](index.md) "Condition typing").

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
[Foreach](#foreach) below); other declarations inside the body are visible only
within it (block scope).

```nlang
int sum = 0;
for (int i = 1; i <= 10; i += 1) {
    sum = sum + i;
}
// sum == 55; `i` is still in scope after the loop (function-scoped)
```

```nlang
int count = 0;
for (int j = 0; j < 20; j += 1) {
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

### Foreach

```nlang
foreach (Type var in iterable) { body }
```

Iterates the elements of `iterable`, binding each to `var` for the body.
Supported iterables:

| Iterable | Iterates | Element access |
|----------|----------|----------------|
| `T[N]` (array) | elements `arr[0]..arr[N-1]` | `arr[i]` |
| `List<T>` | elements in insertion order | `List<T>.get(i)` |
| `Dict<K,V>` | **keys** in insertion order (Python style) | a `dict.keys()` snapshot, then one key per step |
| `string` | **code points** (`char`) | Unicode Transformation Format (UTF-8) decoding, one code point per step |

**Source constraint**: the source expression must be an array, `List`,
`Dict`, or `string` — any shape works: an lvalue, a container- or
array-valued call result (`li.get(0)` on a `List<int[]>`), member access,
`int[n]`, a dict subscript, or a bare array literal (`[1, 2, 3]`, `[]` —
the element type is inferred from the loop variable). The source is
evaluated exactly once, so side-effecting sources run once. Any other
source (an `int` local, a non-container call result) is a compile error:
"the foreach source must be an array, List, Dict, or string".

**String sources**: the loop variable must be `char`, and each iteration
binds one complete code point (multi-byte characters are never split) — this
is the string's code-point iteration path, in contrast with the byte
subscript `s[i]` (see [String](../04-types/string.md) "The char bridge"):

```nlang
string s = "héllo";
int n = 0;
foreach (char c in s) {
    n = n + 1;   // counts code points
}
// n == 5 (contrast s.length() == 6 bytes)
```

**Loop variable typing**: the declared type must match the element type
**exactly** — the element's base type and array dimensions must match
exactly. The variable may be
array-typed: `foreach (int[] row in grid)` where `grid : List<int[]>` binds
each element as an array. Mismatches in either dimension are compile errors:
`foreach (int r in grid)` (element is an array, the variable is not) and
`foreach (float x in nums)` where `nums : List<int>` (numeric widening) both
fail with "the foreach variable type does not match the element type".
Array-typed declarations also work as `for` initializers
(`for (int[] x = arr; ...)`) — the initializer takes effect and the body can
reference the variable.

`break` and `continue` work identically to `for`. The loop variable is
**function-scoped** (loop variables have no block scope, consistent with
the `for` form above):

```nlang
List<int> nums = {};
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// `x` remains in scope here (function-scoped)
```

**Dict iteration example**:

```nlang
Dict<string, int> ages = {};
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**Mutation is undefined behavior**. The element count is taken once at loop entry (a single
`iterable.length()` call).
Structural modifications inside the body (`List.add`/`removeAt`,
`Dict.set`/`remove`) may cause: out-of-bounds access, skipped/duplicated
elements, or stale `keys()` snapshots. Element assignment (`arr[i] = x`)
inside an Array foreach body is fine (no structural change).

**Struct elements are copied into the loop variable** (value semantics):
`foreach (Point p in arr) { p.x = 99; }` does not modify `arr`'s elements —
`p` is a fresh deep copy per iteration (consistent with C#, where foreach
over value-type elements also yields copies). See [Struct](../04-types/struct.md).

**Null iterable** throws null pointer exception (NPE) on the first
`length()` call — consistent with all other class-typed calls. See
[Exceptions](../08-exceptions/index.md).
