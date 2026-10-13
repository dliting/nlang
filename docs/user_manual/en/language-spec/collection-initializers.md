# Collection Initializers


NLang supports collection literals for arrays, lists, dicts, and
aggregate (struct/class) initialization. Three syntactic forms:

**Bare bracket form `[...]`** — allowed where the target type lets the
compiler infer the collection (declaration initializers, assignments,
positional function arguments, container elements). Works for arrays
(`T[]`) and `List<T>`:

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [Point{x:1, y:2}, Point{x:3, y:4}];
```

**Bare brace form `{...}`** — the typed target decides the reading:
keyed entries initialize dicts and struct/class aggregates, positional
entries initialize lists and arrays. It works in the same inference
positions as `[...]`:

```nlang
Dict<string, int> d = {"a":1, "b":2};
List<int> lst = {1, 2, 3};
int[] a = {1, 2};
take({7, 0});
List<int> mk() { return {1, 2}; }
```

**Anchored form `Type{...}`** — a named class or struct head anchors
the literal, so it works in any expression position (function args,
return values, standalone expressions):

```nlang
Point p = Point{x:1, y:2};
return Point{x:0, y:0};
foo(Point{x:1, y:2}, Point{x:3, y:4});
```

Generic instantiation heads do not anchor — `List<int>{60}` does not
parse; give the literal a typed target instead (`List<int> l = {60};`).

**Entry forms inside `{...}`:**

- `string literal : Expression` — dict entry (string key)
- `identifier : Expression` — struct/class field (e.g. `x:1, y:2`)
- `Expression` (no key) — list or array element (only valid when the
  target is `List<T>` or `T[]`)

**Dict keys must be string literals**: the collection initializer emits
each key as a string constant and boxes it by K's tag — this is only
correct for `K = string`. A `Dict<K,V>` with a non-string key and a
non-empty initializer is a compile error (`Dict<int, int> d = {"1": 2}`
reports `the Dict collection initializer requires string keys; use set()
with an explicit 'Int32' key`); the empty initializer
`Dict<int, int> d = {}` is legal — fill entries afterwards with
`set()`.

**Type disambiguation:** the compiler uses the typed target (the
declared variable, the parameter type, the return type) or the
anchored type name to pick the kind:

| Target type          | Form    | Entry kind              |
|----------------------|---------|-------------------------|
| `T[]` (array)        | `[...]` or `{...}` | value-only |
| `List<T>`            | `[...]` or `{...}` | value-only |
| `Dict<K,V>`          | `{...}` | `key : value` (string key) |
| struct               | `{...}` or `StructName{...}` | `field : value` (identifier key) |
| class                | `{...}` or `ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `C{f1:v1, ...}` as a no-arg
construction followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists. The
inner levels use the bracket form (`List<List<int>> ll = [[1,2],[3,4]];`)
— bare braces nested inside a brace literal do not parse
(`{{1,2},{3,4}}` is a syntax error). Nested generic element types
(`List<List<int>>`, `Dict<K, List<V>>`) are supported (see
Nested generics under `List<T>` in
[Built-in Generic Classes](builtin-generic-classes.md)).

**Empty collections:** the empty literal is `{}` for every container
target — `List<int> l = {};`, `Dict<K,V> d = {};`, `int[] a = {};`.
Bare `[]` is not the empty form (the lexer matches `[]` as a single
token, used by the array-type suffix grammar); for a sized empty array
write `int[0]` (see [Array](array.md)).

**Function arguments:** a positional argument infers its target from
the parameter type (`take({7, 0})` for a `Point` parameter). A named
argument does not infer — bind a typed local first, or use the anchored
form.

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from within
an entry expression (e.g. `[1, foo(arr)]` where `foo` reads `arr`) is
undefined behavior. An exception-throwing entry leaves the collection
partially constructed.
