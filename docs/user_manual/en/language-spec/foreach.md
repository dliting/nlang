# Foreach


```nlang
foreach (Type var in iterable) { body }
```

Iterates the elements of `iterable`, binding each to `var` for the body.
Supported iterables:

| Iterable | Iterates | Element access |
|----------|----------|----------------|
| `T[N]` (array) | elements `arr[0]..arr[N-1]` | `OP_LoadElement` |
| `List<T>` | elements in insertion order | `List<T>.get(i)` |
| `Dict<K,V>` | **keys** (Python style) | inline `dict.keys()` then `List<K>.get(i)` |
| `string` | **code points** (`char`) | Unicode Transformation Format (UTF-8) decode advance |

**Source constraint**: the source expression must be an array, `List`,
`Dict`, or `string` — any shape works: an lvalue, a container- or
array-valued call result (`li.get(0)` on a `List<int[]>`), member access,
`new int[n]`, a dict subscript. The source is evaluated exactly once (bound
to a hidden iteration local), so side-effecting sources run once. Any other
source (an `int` local, a non-container call result) is a compile error:
"the foreach source must be an array, List, Dict, or string".

**String sources**: the loop variable must be `char`, and each iteration
binds one complete code point (multi-byte characters are never split) — this
is the string's code-point iteration path, in contrast with the byte
subscript `s[i]` (see [String](string.md) "The char bridge"):

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
`for` — see [Loops](loops.md)):

```nlang
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// `x` remains in scope here (function-scoped)
```

**Dict iteration example**:

```nlang
Dict<string, int> ages = new Dict<string, int>();
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**Mutation is undefined behavior**. The element count is cached at loop entry
(`n = iterable.length()` for List/Dict, `n = arr.length` for Array).
Structural modifications inside the body (`List.add`/`removeAt`,
`Dict.set`/`remove`) may cause: out-of-bounds access, skipped/duplicated
elements, or stale `keys()` snapshots. Element assignment (`arr[i] = x`)
inside an Array foreach body is fine (no structural change).

**Struct elements are copied into the loop variable** (value semantics):
`foreach (Point p in arr) { p.x = 99; }` does not modify `arr`'s elements —
`p` is a fresh deep copy per iteration (consistent with C#, where foreach
over value-type elements also yields copies). See [Struct](struct.md).

**Null iterable** throws null pointer exception (NPE) on the first
`length()` call — consistent with all other class-typed calls. See
[Exceptions](exception.md).
