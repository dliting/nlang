# Built-in Generic Classes


### `List<T>`

`List<T>` is a growable, ordered, index-addressable collection. It is
a **built-in generic class** — only `List` (and future `Dict`) are
recognized by the compiler; user-defined `class Foo<T>` is not (yet)
supported.

```nlang
List<int> nums = new List<int>();
nums.add(1);
nums.add(2);
nums.add(3);
int sum = nums.get(0) + nums.get(1) + nums.get(2);    // 6
int n = nums.length();                                 // 3

List<string> names = new List<string>();
names.add("alice");
names.add("bob");
int total = (names.get(0) + names.get(1)).length();    // 8
```

**Methods** (T is the element type):

| Method              | Signature          | Returns | Notes                              |
|---------------------|--------------------|---------|------------------------------------|
| `add`               | `void add(T item)` | —       | Append to end                      |
| `get`               | `T get(int idx)`   | T       | Read by index; throws if OOB       |
| `set`               | `void set(int i, T)` | —     | Overwrite element                  |
| `length`            | `int length()`     | int     | Current element count              |
| `removeAt`          | `void removeAt(int i)` | —   | Erase; shifts later elements down  |
| `indexOf`           | `int indexOf(T item)` | int | First index of `item`, or -1       |
| `contains`          | `int contains(T item)` | int | 1 if present else 0               |
| `clear`             | `void clear()`     | —       | Remove all elements                |

**Type checking**: the compiler recognizes `List<int>`, `List<string>`,
`List<Point>`, etc. as distinct static types. Argument types are checked
against the substituted signature — `nums.add("wrong")` is a compile
error when `nums : List<int>`.

**Erasure runtime model**: `List<int>` and `List<Point>` share the same
backing class at runtime. Elements are stored uniformly as heap indices
in a side table; primitive elements are boxed via
`OP_Box` at the call site. GC traces list elements as additional roots.

**Array type arguments**: `T` may be an array type — `List<int[]>`
stores `int[]` values as raw, GC-traced handles; the primitive-boxing
rule above does not apply to array-typed elements. Elements pulled out
with `get`/subscript keep their array-ness for the compiler's gates,
and `foreach (int[] row in grid)` iterates them directly. `indexOf`/
`contains` compare by handle identity. Jagged arguments
(`List<int[][]>`) are rejected like other jagged declarations.

**Null List reference**: a `List<T>` field or variable that has not been
assigned `new List<T>()` holds null. Calling any method on null throws
`null reference in CallMethod` (same NPE semantics as other class refs).

**Collection initializer**: the `[1, 2, 3]` literal syntax is supported
(bare bracket form for arrays and `List<T>`). See the
Collection Initializers section above.

**`foreach`**: the `foreach (Type var in iterable)` construct is
supported. See the Foreach Statement section below.

**Nested generics** (`List<List<int>>`): supported. The lexer tracks
type-argument nesting depth (`<` right after the built-in generic names
`List`/`Dict` opens a level, each `>` closes one) and splits `>>` into
two `'>'` tokens while the depth is positive (C#-style scanner split),
so `List<List<int>>` and `Dict<string, List<int>>` parse. Outside
generic context `>>` remains a single token (not split) — right-shift has
no grammar production, so `x >> 2` is a compile error (the shift operator
itself is not implemented). Limitation: a comparison against a variable
shadowing the type name (`List < 3`, only blanks/comments between name
and `<`) is misread as a generic open.

**Cross-module limitation**: container generic signatures do not cross
`.nmod` import boundaries — imported function signatures serialize each
type as a single kind byte, so a `List<int[]>` parameter or return in
an imported function degrades to a plain class reference and the
array-ness is not reachable. Plain `T[]` signatures do cross (serialized
as an array kind); container instantiations do not.

### `Dict<K,V>`

`Dict<K,V>` is an associative array mapping keys of type `K` to values
of type `V`. Like `List<T>`, it is a **built-in generic class** — only
`List` and `Dict` are recognized by the compiler; user-defined generics
are not (yet) supported.

```nlang
Dict<string,int> scores = new Dict<string,int>();
scores.set("alice", 90);
scores.set("bob",   85);
int a = scores.get("alice");          // 90
int hasBob = scores.containsKey("bob"); // 1
int n = scores.count();                 // 2

Dict<int,int> squares = new Dict<int,int>();
squares.set(3, 9);
squares.set(4, 16);
squares.set(3, 99);                     // overwrites 9 → 99
int v = squares.get(3);                 // 99
int removed = squares.remove(4);        // 1
```

**Methods** (K is the key type, V is the value type):

| Method           | Signature                  | Returns | Notes                                          |
|------------------|----------------------------|---------|------------------------------------------------|
| `set`            | `void set(K key, V value)` | —       | Insert-or-replace (no duplicate-key error)     |
| `get`            | `V get(K key)`             | V       | Lookup; **throws** if key absent               |
| `containsKey`    | `int containsKey(K key)`   | int     | 1 if present, 0 otherwise                      |
| `remove`         | `int remove(K key)`        | int     | 1 if removed, 0 if key not found               |
| `clear`          | `void clear()`             | —       | Remove all entries                             |
| `count`          | `int count()`              | int     | Current entry count                            |

**Type checking**: the compiler recognizes `Dict<int,int>`,
`Dict<string,Point>`, etc. as distinct static types. Argument types are
checked against the substituted signature — `d.set("x", "y")` is a
compile error when `d : Dict<string,int>`.

**Erasure runtime model**: `Dict<K,V>` shares a single backing class
across all instantiations. Entries are stored as `(K heap idx, V heap
idx)` pairs in a side table; primitive keys/values are
boxed via `OP_Box` at the call site. GC traces every entry's K and V as
additional roots.

**Key equality** is kind-aware:
- Primitive keys (boxed `int`, `float`): compare value bits (IEEE 754 —
  `NaN != NaN`, documented behavior).
- `string` keys: compare string content (value equality).
- `class` / `struct` keys: compare heap idx (identity), matching Java's
  `IdentityHashMap` and C#'s default `object.Equals`. A user `Equals`
  override is **not** consulted — override-based key-equality semantics
  are not supported.
- Array keys (`Dict<int[], V>`): compare handle identity — two separate
  `int[2]` arrays with equal contents are different keys.

**Null Dict reference**: a `Dict<K,V>` field or variable that has not
been assigned `new Dict<K,V>()` holds null. Calling any method on null
throws `NLang VM: Dict <method> on null instance`.

**Linear-scan lookup (current limitation)**: every `set`/`get`/
`containsKey`/`remove` does an O(n) scan of the entries vector. This is
acceptable for typical small scripts; O(1) hashtable lookup is not
provided.

**`foreach` over keys**: `foreach (K k in dict) { ... }`
iterates the keys of the dict, Python/JavaScript style. Inside the body,
call `dict.get(k)` to access the value. Implementation: the compiler
emits an inline `dict.keys()` call to materialize a fresh `List<K>`, then
iterates that list. See the Foreach Statement section below.

**`Dict.keys()`**: returns a new `List<K>` populated with all keys
(no defined ordering). Useful independently of `foreach` for snapshotting
keys for enumeration, set-style membership checks via `contains`, etc.
The returned `List<K>` is a *copy* — subsequent `set`/`remove` on the
source dict do not affect it.

**`values()`**: not yet provided. Iterate keys and call `get` to obtain
values.

### Subscript Sugar — `li[i]` / `d[k]`

Both containers support subscript syntax as pure sugar over the
`get` / `set` intrinsics (zero new opcodes):

```nlang
List<int> li = new List<int>();
li.add(2);
li.add(5);
int v = li[1];          // == li.get(1)      -> 5
li[0] = 9;              // == li.set(0, 9)

Dict<string, int> d = new Dict<string, int>();
d["a"] = 3;             // == d.set("a", 3)
int x = d["a"];         // == d.get("a")     -> 3
```

Out-of-range reads/writes and missing dict keys throw exactly as the
method forms do (IndexOutOfBoundsException family).

Because the compiler peels the element type T/V off the container
type, subscripts compose with the rest of the language:

```nlang
List<List<int>> m = ...;
m[0][1] = 47;           // chained subscript write

List<Point> pts = ...;
pts[0].x = 9;           // member write through a subscript receiver
int s = pts[0].x + pts[0].y;

List<int>[] arr = new List<int>[2];   // arrays of generic instantiations
arr[0] = new List<int>();
arr[0][1];              // container subscript through an array element
```

Compound subscript assignment (`li[0] += 1`) is intentionally not
supported (same policy as arrays); write `li[0] = li[0] + 1`.
