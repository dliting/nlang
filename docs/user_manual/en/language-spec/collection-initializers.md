# Collection Initializers


NLang supports collection literals for arrays, lists, dicts, and
aggregate (struct/class) initialization. Three syntactic forms:

**Bare bracket form `[...]`** — allowed where the target type lets the
compiler infer the collection (declaration initializers, assignments,
return values, `foreach` sources, positional and named function
arguments, container elements). Works for arrays (`T[]`) and `List<T>`:

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [Point{x:1, y:2}, Point{x:3, y:4}];
```

**Bare brace form `{...}`** — the typed target decides the reading:
keyed entries initialize dicts and struct/class aggregates, positional
entries initialize lists. It works in the same inference positions as
`[...]`, except as a `foreach` source — there the target is always the
loop variable's array type, and the brace form is rejected:

```nlang
Dict<string, int> d = {"a":1, "b":2};
List<int> lst = {1, 2, 3};
take({7, 0});
count(items = {1, 2});
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
- `Expression` (no key) — list element, or a positional struct
  entry: structs fill fields in declaration order, so `Point{3, 7}` is
  `Point{x:3, y:7}`, and you may give only the first few fields (the
  rest keep their default values). Class targets do not accept positional
  entries — `P{3, 7}` reports `class initializer "P{...}" requires
  field:value entries; the positional form is only valid for
  struct/array targets`.

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
| `T[]` (array)        | `[...]`            | value-only |
| `List<T>`            | `[...]` or `{...}` | value-only |
| `Dict<K,V>`          | `{...}` | `key : value` (string key) |
| struct               | `{...}` or `StructName{...}` | `field : value` (identifier key) or positional values (field declaration order) |
| class                | `{...}` or `ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `C{f1:v1, ...}` as a no-arg
construction followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists. Inner
levels may use the bracket form (`List<List<int>> ll = [[1,2],[3,4]];`)
or bare braces in value position (`List<List<int>> m = {{1,2},{3,4}};`).
Keyed entries still require an expression value — `{items: {1,2}}`
inside a struct/class literal is a syntax error; write
`{items: [1,2]}`. Nested generic element types
(`List<List<int>>`, `Dict<K, List<V>>`) are supported (see
Nested generics under `List<T>` in
[Built-in Generic Classes](builtin-generic-classes.md)).

**Empty collections:** the empty literal is `{}` for `List<T>` and
`Dict<K,V>` targets — `List<int> l = {};`, `Dict<K,V> d = {};`. An
array target takes no brace form (`int[] a = {};` is rejected — array
initializers use the bracket form); an empty array is `int[] a = [];`
(see [Array](array.md)). The empty bracket literal also works as a
`foreach` source and as a function argument.

**Function arguments:** a positional argument infers its target from
the parameter type — positional entries work for struct and `List<T>`
parameters (`take({7, 0})` for a struct `Point` parameter); an array
parameter takes the bracket form (`take([7, 0])`); a class parameter
takes `field:value` entries (see the table above). A named argument
infers the same way (`count_of(items = {1, 2, 3})` for a `List<int>`
parameter).

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from within
an entry expression (e.g. `[1, foo(arr)]` where `foo` reads `arr`) is
undefined behavior. An exception-throwing entry leaves the collection
partially constructed.
