# Collection Initializers


NLang supports collection literals for arrays, lists, dicts, and
aggregate (struct/class) initialization. Three syntactic forms:

**Bare bracket form `[...]`** — allowed where the target type lets the
compiler infer the collection (declaration initializers, assignments,
return values, `foreach` sources, positional and named function
arguments, container elements). At target-less positions (method
receivers, subscript bases) the element type is inferred from the
entries (empty literals and mixed-type entries still need a typed
target). Works for arrays (`T[]`) and `List<T>`:

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [Point{x:1, y:2}, Point{x:3, y:4}];
```

**Bare brace form `{...}`** — the typed target decides the reading:
keyed entries initialize dicts and struct/class aggregates, positional
entries initialize lists. It works in the same inference positions as
`[...]`; as a `foreach` source it infers `List<T>` from the loop
variable's type:

```nlang
Dict<string, int> d = {"a":1, "b":2};
List<int> lst = {1, 2, 3};
take({7, 0});
count(items = {1, 2});
List<int> mk() { return {1, 2}; }
```

**Anchored form `Type{...}`** — a named class, struct, or built-in
generic head anchors the literal, so it works in any expression
position (function args, return values, standalone expressions):

```nlang
Point p = Point{x:1, y:2};
return Point{x:0, y:0};
foo(Point{x:1, y:2}, Point{x:3, y:4});
```

Generic instantiation heads anchor too — `List<int>{60}` and
`Dict<string, int>{"a": 1}` parse in any expression position; the
anchored form is limited to the built-in generic names (`List`, `Dict`,
and `Channel` — a `Channel` initializer must be empty, and `Future`
accepts no collection initializer in any form).

**Entry forms inside `{...}`:**

- `Expression : Expression` — keyed entry. In a `Dict<K,V>` target the
  key is a full expression checked against K (see Dict keys below). In
  a struct/class target the key must be a bare identifier naming a
  field (e.g. `x:1, y:2`) — there the identifier is a field name, not a
  variable
- `Expression` (no key) — list element, or a positional struct
  entry: structs fill fields in declaration order, so `Point{3, 7}` is
  `Point{x:3, y:7}`, and you may give only the first few fields (the
  rest keep their default values). Class targets do not accept positional
  entries — `P{3, 7}` reports `class initializer "P{...}" requires
  field:value entries; the positional form is only valid for
  struct/array targets`.

**Entry form must match the target kind**: a mismatched entry is a
compile error — a keyed entry in a `List<T>` or array initializer
(`List<int> l = {x: 1};` reports `a List initializer takes positional
entries; the keyed form is only valid for Dict, struct, and class
initializers`, and `int[] arr = [1: 2];` reports `an array initializer
takes positional entries; the keyed form is only valid for Dict, struct,
and class initializers`), a keyless entry in a `Dict<K,V>` initializer
(`Dict<string, int> d = {1, 2};` reports `a Dict initializer requires a
key in every entry; positional entries are only valid for List, struct,
and array initializers`), and an expression key in a struct or class
initializer (`S s = {"a": 1};` reports `struct initializer "S{...}" keys
are field names; an expression key is only valid for Dict initializers`,
and `C c = {f(): 1};` reports `class initializer "C{...}" requires
field:value entries; the positional form is only valid for struct/array
targets`).

**Dict keys are expressions**: every entry of a `Dict<K,V>` initializer
carries a key expression, checked against the declared key type K by the
same conversion rule as values — literals, variables, calls, member
accesses and compound expressions all work
(`Dict<int, string> d = {1: "one", n: "ten"};` and
`Dict<string, int> d = {k: 7, c.name: 8, a + "y": 3};`). A key whose
type the conversion matrix does not admit is a compile error
(`Dict<int, int> d = {"a": 1};` reports `Incompatible type "a".`). Keys
evaluate before their values and entries left-to-right; a duplicate key
overwrites the earlier entry — the same `set()` semantics as at
runtime.

**Type disambiguation:** the compiler uses the typed target (the
declared variable, the parameter type, the return type) or the
anchored type name to pick the kind:

| Target type          | Form    | Entry kind              |
|----------------------|---------|-------------------------|
| `T[]` (array)        | `[...]`            | value-only |
| `List<T>`            | `[...]` or `{...}` | value-only |
| `Dict<K,V>`          | `{...}` | `key : value` (key expression checked against K) |
| struct               | `{...}` or `StructName{...}` | `field : value` (identifier key) or positional values (field declaration order) |
| class                | `{...}` or `ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `C{f1:v1, ...}` as a no-arg
construction followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists. Inner
levels may use the bracket form (`List<List<int>> ll = [[1,2],[3,4]];`)
or bare braces in value position (`List<List<int>> m = {{1,2},{3,4}};`).
Keyed entries accept bare brace values too (`{"a": {1, 2}}` for a
`Dict<string, List<int>>` target, `{items: {1, 2}}` for a `List<int>`
field); array-typed fields keep the bracket form (`{items: [1, 2]}`).
Nested generic element types
(`List<List<int>>`, `Dict<K, List<V>>`) are supported (see
Nested generics under `List<T>` in
[Built-in Generic Classes](builtin-generic-classes.md)).

**Empty collections:** the empty literal is `{}` for `List<T>` and
`Dict<K,V>` targets — `List<int> l = {};`, `Dict<K,V> d = {};`. An
array target takes no brace form (`int[] a = {};` is rejected — array
initializers use the bracket form); an empty array is `int[] a = [];`
(see [Array](array.md)). The empty bracket literal also works as a
`foreach` source and as a function argument. At a target-less position
the empty bracket literal has no element type to infer (`[].length()`
is rejected); give it a typed target.

**Function arguments:** a positional argument infers its target from
the parameter type — positional entries work for struct and `List<T>`
parameters (`take({7, 0})` for a struct `Point` parameter); an array
parameter takes the bracket form (`take([7, 0])`); a class parameter
takes `field:value` entries (see the table above). A named argument
infers the same way (`count_of(items = {1, 2, 3})` for a `List<int>`
parameter).

This inference covers free-function calls only. A method-call argument
carries no inference target — `m.add({1, 2})` on a `List<List<int>>`
reports `Collection initializer requires an explicit type or an LHS
context to infer the target type.`; pass an anchored literal
(`m.add(List<int>{1, 2})`) or a variable instead.

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from within
an entry expression (e.g. `[1, foo(arr)]` where `foo` reads `arr`) is
undefined behavior. An exception-throwing entry leaves the collection
partially constructed.
