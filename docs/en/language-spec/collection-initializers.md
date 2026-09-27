# Collection Initializers


NLang supports C-style collection literals for arrays, lists, dicts, and
aggregate (struct/class) initialization. Two syntactic forms:

**Bare bracket form `[...]`** — allowed only where the LHS or assignment
target lets the compiler infer the collection type. Works for arrays
(`T[]`) and `List<T>`:

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [new Point{x:1, y:2}, new Point{x:3, y:4}];
```

**Explicit form `new Type{...}`** — works in any expression position
(function args, return values, standalone expressions). Required for dict,
struct, and class initialization because bare `{...}` would conflict with the
block-statement grammar (a `{...}`-surrounded statement group):

```nlang
Dict<string, int> d = new Dict<string, int>{"a":1, "b":2};
Point p = new Point{x:1, y:2};
List<int> lst = new List<int>{1, 2, 3};
return new Point{x:0, y:0};
foo(new Point{x:1, y:2}, new Point{x:3, y:4});
```

**Entry forms inside `{...}`:**

- `string literal : Expression` — dict entry (string key)
- `identifier : Expression` — struct/class field (e.g. `x:1, y:2`)
- `Expression` (no key) — list element (only valid when Type is `List<T>`)

**Type disambiguation:** the compiler uses the LHS variable (or the explicit
`Type` in `new Type{...}`) to pick the kind:

| Target type          | Form    | Entry kind              |
|----------------------|---------|-------------------------|
| `T[]` (array)        | `[...]` | value-only              |
| `List<T>`            | `[...]` or `new List<T>{...}` | value-only |
| `Dict<K,V>`          | `new Dict<K,V>{...}` | `key : value` (string key) |
| struct               | `new StructName{...}` | `field : value` (identifier key) |
| class                | `new ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `new C{f1:v1, ...}` as `new C()`
followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists. Nested generic
element types (`List<List<int>>`, `Dict<K, List<V>>`) are supported (see
Nested generics under `List<T>` in
[Built-in Generic Classes](builtin-generic-classes.md)).

**Empty collections:** bare `[]` is not supported (the lexer matches `[]` as
a single token, used by the array-type suffix grammar). Use the explicit empty
form instead: `new List<T>{}`, `new Dict<K,V>{}`, or `new int[0]` for arrays
(see [Array](array.md)).

**Function arg disambiguation:** a bare `[...]` as a function argument is not
currently supported — it produces a compile error because the compiler cannot
infer the target type without overload resolution. Use the explicit
`new Type{...}` form for function args.

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from within
an entry expression (e.g. `[1, foo(arr)]` where `foo` reads `arr`) is
undefined behavior. An exception-throwing entry leaves the collection
partially constructed.
