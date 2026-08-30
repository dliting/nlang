# Built-in Generic Classes


### `List<T>` — Phase 8e-3

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
| `Add`               | `void Add(T item)` | —       | Append to end                      |
| `Get`               | `T Get(int idx)`   | T       | Read by index; throws if OOB       |
| `Set`               | `void Set(int i, T)` | —     | Overwrite element                  |
| `Length`            | `int Length()`     | int     | Current element count              |
| `RemoveAt`          | `void RemoveAt(int i)` | —   | Erase; shifts later elements down  |
| `IndexOf`           | `int IndexOf(T item)` | int | First index of `item`, or -1       |
| `Contains`          | `int Contains(T item)` | int | 1 if present else 0               |
| `Clear`             | `void Clear()`     | —       | Remove all elements                |

**Type checking**: the compiler recognizes `List<int>`, `List<string>`,
`List<Point>`, etc. as distinct static types. Argument types are checked
against the substituted signature — `nums.add("wrong")` is a compile
error when `nums : List<int>`.

**Erasure runtime model**: `List<int>` and `List<Point>` share the same
backing class at runtime. Elements are stored uniformly as heap indices
in a side table (`m_listStore`); primitive elements are boxed via
`OP_Box` at the call site. GC traces list elements as additional roots.

**Null List reference**: a `List<T>` field or variable that has not been
assigned `new List<T>()` holds null. Calling any method on null throws
`null reference in CallMethod` (same NPE semantics as other class refs).

**Collection initializer**: `[1, 2, 3]` literal syntax is supported
since Phase 8e-6 (bare bracket form for arrays and `List<T>`). See the
Collection Initializers section above.

**`foreach`**: the `foreach (Type var in iterable)` construct is supported
since Phase 8e-5. See the Foreach Statement section below.

**Nested generics** (`List<List<int>>`): supported. The lexer tracks
type-argument nesting depth (`<` right after the built-in generic names
`List`/`Dict` opens a level, each `>` closes one) and splits `>>` into
two `'>'` tokens while the depth is positive (C#-style scanner split),
so `List<List<int>>` and `Dict<string, List<int>>` parse. Outside
generic context `>>` remains a single `OT_RSH` token — right-shift has
no production, so `x >> 2` is a compile error (the shift operator
itself is not implemented). Limitation: a comparison against a variable
shadowing the type name (`List < 3`, only blanks/comments between name
and `<`) is misread as a generic open.

### `Dict<K,V>` — Phase 8e-4

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
| `Set`            | `void Set(K key, V value)` | —       | Insert-or-replace (no duplicate-key error)     |
| `Get`            | `V Get(K key)`             | V       | Lookup; **throws** if key absent               |
| `ContainsKey`    | `int ContainsKey(K key)`   | int     | 1 if present, 0 otherwise                      |
| `Remove`         | `int Remove(K key)`        | int     | 1 if removed, 0 if key not found               |
| `Clear`          | `void Clear()`             | —       | Remove all entries                             |
| `Count`          | `int Count()`              | int     | Current entry count                            |

**Type checking**: the compiler recognizes `Dict<int,int>`,
`Dict<string,Point>`, etc. as distinct static types. Argument types are
checked against the substituted signature — `d.set("x", "y")` is a
compile error when `d : Dict<string,int>`.

**Erasure runtime model**: `Dict<K,V>` shares a single backing class
across all instantiations. Entries are stored as `(K heap idx, V heap
idx)` pairs in a side table (`m_dictStore`); primitive keys/values are
boxed via `OP_Box` at the call site. GC traces every entry's K and V as
additional roots.

**Key equality** is kind-aware:
- Primitive keys (boxed `int`, `float`): compare value bits (IEEE 754 —
  `NaN != NaN`, documented behavior).
- `string` keys: compare string-pool content (value equality).
- `class` / `struct` keys: compare heap idx (identity), matching Java's
  `IdentityHashMap` and C#'s default `object.Equals`. A user `Equals`
  override is **not** consulted — override-based dictionary semantics
  are a separate future phase.

**Null Dict reference**: a `Dict<K,V>` field or variable that has not
been assigned `new Dict<K,V>()` holds null. Calling any method on null
throws `NLang VM: Dict <method> on null instance`.

**Linear-scan lookup (current limitation)**: every `Set`/`Get`/
`ContainsKey`/`Remove` does an O(n) scan of the entries vector. This is
acceptable for typical small scripts; O(1) hashtable lookup is a future
optimization phase.

**`foreach` over keys (Phase 8e-5)**: `foreach (K k in dict) { ... }`
iterates the keys of the dict, Python/JavaScript style. Inside the body,
call `dict.get(k)` to access the value. Implementation: codegen emits
an inline `dict.keys()` call to materialize a fresh `List<K>`, then
iterates that list. See the Foreach Statement section below.

**`Dict.keys()`**: returns a new `List<K>` populated with all keys
(no defined ordering). Useful independently of `foreach` for snapshotting
keys for enumeration, set-style membership checks via `Contains`, etc.
The returned `List<K>` is a *copy* — subsequent `Set`/`Remove` on the
source dict do not affect it.

**`Values()`**: not yet provided. Iterate keys and call `Get` to obtain
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

Because the resolver peels the element type T/V off the container
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
