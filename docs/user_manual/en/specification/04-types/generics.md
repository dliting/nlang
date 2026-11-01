# 4.12 Generics

## 4.12.1 Generic classes and instantiation

A class declared with *type parameters* — `class Box<T> { ... }`,
`class Pair<K, V> { ... }` — is a *generic class*. Using one
supplies *type arguments* in angle brackets after the name:
`Box<int>`, `Pair<string, int>`. The standard library's containers
are generic classes: `List<T>` and `Dict<K, V>` are prelude names,
visible without an import (§4.12.6); `Channel<T>` (package
`concurrent`) and `Future<T>` (package `thread`) are used the same
way.

Each instantiation is a distinct type (§4.1): `List<int>` and
`List<string>` are different, mutually incompatible types, and so
are `Dict<string, int>` and `Dict<string, long>`. Calls are checked
against the substituted signature: with `nums` declared
`List<int>`, `nums.add("wrong")` is a compile-time error.

A generic instance is an ordinary class instance: a reference-type
value, default `null`, assignable to `Object` (§4.8, §4.11).

*Informative:* a generic class compiles once as one template row,
and every instantiation shares it. Container elements are stored
uniformly as references, and scalar primitive elements are boxed on
insertion (§4.11.3); the garbage collection traces container
elements as additional roots.

## 4.12.2 Declaring a generic class

Type parameters are declared in angle brackets after the class name
(§2.4) and name ordinary types everywhere a type may appear: field,
parameter, return and local types. Members carry bodies like any
other class — methods and constructors compile once against the
type parameters. Every method of a generic class must be `native`
or have a body. A member name must not repeat a type parameter
name.

```nlang
class Pair<K, V> {
    public K key;
    public V value;
    public Pair(K k, V v) {
        this.key = k;
        this.value = v;
    }
    V get() { return this.value; }
}
```

A field whose type is a bare type parameter cannot carry an
initializer: the type parameter has no concrete kind, so no initial
value exists; a constructor may assign it. A field of a concrete
type may carry an initializer like any field (§4.8.3). Likewise a
formal whose type is a bare type parameter cannot carry a default
value (§7.2). A type-parameter field that is never assigned reads
as the default value of the type argument it was instantiated with:
`0` for an integer argument, `null` for a `string` or class
argument.

A type parameter may be an array element type in a type position —
`T[]` is a legal field type — but array creation over a type
parameter is rejected: the element kind is not known until the
instantiation (§4.10.2).

*Informative:* a default argument on a generic member carries into
instantiations only in constant-expression shapes — a literal, an
operator tree over constants, or a reference to a constant, formal,
field or enum member. Any other shape, a call expression or a cast,
is not carried: a call through an instantiation that omits such an
argument reports the argument missing.

## 4.12.3 Construction

A generic instance is created by a constructor call written with the
bare template name against a declared target type:

```nlang
class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

int main() {
    Box<int> b = Box(7);    // the instantiation comes from the declared type
    return b.get();
}
```

The bare name alone carries no type arguments: the instantiation is
read from the declared target — the declaration being initialized,
the assignment target or the parameter type. A bare-name
constructor call with no declared target is a compile-time error
(`generic class "Box" cannot be constructed by bare name`). A
generic class with no declared constructor takes the zero-argument
form the same way: `Cell<char> cc = Cell();`.

The containers also construct through the initializer forms:
`List<int> nums = {};` and `List<int> seed = List();` are
equivalent, and the bracket literal `[1, 2, 3]` fills a list from a
literal (see [Collection
Initializers](../05-expressions/initializers.md)).

## 4.12.4 Nesting and recursive references

A type argument may itself be an instantiation: `Box<Box<int>>`,
`List<List<int>>`, `Dict<string, List<int>>`. Inside a
type-argument list `>>` splits into two `>` tokens; outside one it
remains the right-shift operator (chapter 5).

A generic class may refer to itself with the same type arguments —
`class Node<T> { public Node<T> next; }` — and two generic classes
may refer to each other the same way. A field type that nests a
fresh instantiation of the class being declared is rejected:
`class A<T> { public A<A<T>> next; }` reports the
instantiation-depth error.

*Informative:* the compiler mints at most 16 nested instantiations;
exceeding the limit reports `The generic instantiation depth
exceeds the limit of 16 nested instantiations.`

## 4.12.5 Type arguments

A type argument may be a scalar primitive, `string`, an enumeration,
a class type, an interface type, an array type, or another generic
instantiation. A `void` type argument is a compile-time error
(§4.13).

A user-declared generic class additionally rejects struct and
function type arguments: a struct is a multi-slot value and a
generic field stores one value slot; a function value is a
multi-slot record (target, receiver, form). The built-in containers
accept struct type arguments (a struct key in a `Dict` compares by
identity, §4.12.6).

An array type argument — `List<int[]>` — stores the array values as
raw references; the primitive-boxing rule does not apply to them.
Elements pulled out with `get` or subscript keep their array type,
so `foreach (int[] row in grid)` iterates them directly (chapter
6). `indexOf` and `contains` compare array elements by handle
identity. A jagged type argument (`List<int[][]>`) is rejected like
any jagged declaration (§4.10.1).

An instantiation works as an element type in the declaration-site
array form — `List<int> arr[2];` — while the creation form
`List<int>[2]` in an expression is a syntax error (§4.10.2). A type
alias may name an instantiation (§3.4): after `using IL =
List<int>;`, `IL` works as a type and as a construction target.

## 4.12.6 Built-in containers List and Dict

`List<T>` is a growable, ordered, index-addressable collection of
elements of type `T`.

| Method       | Signature            | Returns | Notes                             |
|--------------|----------------------|---------|-----------------------------------|
| `add`        | `void add(T item)`   | —       | Append to end                     |
| `get`        | `T get(int idx)`     | T       | Read by index; raises if out of bounds |
| `set`        | `void set(int i, T)` | —       | Overwrite element                 |
| `length`     | `int length()`       | int     | Current element count             |
| `removeAt`   | `void removeAt(int i)` | —     | Erase; shifts later elements down |
| `indexOf`    | `int indexOf(T item)` | int    | First index of `item`, or -1      |
| `contains`   | `bool contains(T item)` | bool | true if present, else false       |
| `clear`      | `void clear()`       | —       | Remove all elements               |

`get` with an index outside the current length raises an
`IndexOutOfBoundsException` (chapter 9).

`Dict<K, V>` is an associative array mapping keys of type `K` to
values of type `V`.

| Method         | Signature                  | Returns | Notes                                      |
|----------------|----------------------------|---------|--------------------------------------------|
| `set`          | `void set(K key, V value)` | —       | Insert or replace (no duplicate-key error) |
| `get`          | `V get(K key)`             | V       | Lookup; raises if the key is absent        |
| `containsKey`  | `bool containsKey(K key)`  | bool    | true if present, else false                |
| `remove`       | `int remove(K key)`        | int     | 1 if removed, 0 if the key was not found   |
| `clear`        | `void clear()`             | —       | Remove all entries                         |
| `count`        | `int count()`              | int     | Current entry count                        |

`get` for a key the map does not hold raises an `Exception`
(chapter 9); `set` inserts or replaces and never fails on a
duplicate key.

`Dict` key equality is kind-aware:

- scalar primitive keys compare value bits, with `NaN` not equal to
  itself;
- `string` keys compare content (§4.3);
- class, struct and array keys compare identity — a user `equals`
  override is not consulted (§4.11.2).

`Dict.keys()` returns a new `List<K>` holding every key in
insertion order; the list is a copy, so later `set`/`remove` on the
dict do not affect it. `foreach (K k in dict)` iterates the keys the
same way, through one `keys()` snapshot (chapter 6). `values()` is
not provided; iterate the keys and call `get`.

Container methods take named arguments under the ordinary rule
(§7.2): `l.add(item = 5)`, `d.set(key = "a", value = 1)`.

Calling any method through a `null` container reference raises a
catchable `NullPointerException` with message
`NLang VM: null reference error`, as through any other class
reference (chapter 9, §4.8.10).

With `T` written as `Object`, a container holds a mix of value
types: primitives and `string` box implicitly at `add`/`set`, class
instances store their references, and an element pulled out has
static type `Object` — unbox it with `as` to use it as a concrete
type (§4.11.4). `contains` and `indexOf` compare boxed primitive
elements by value. The same applies to `Dict` with `K` or `V`
written as `Object`.

```nlang
int main() {
    List<Object> bag = {};
    bag.add(42);                     // int boxed in
    bag.add("hi");                   // string alongside
    int back = bag.get(0) as int;    // 42
    if (back == 42 && bag.contains(42)) {
        return 42;
    }
    return 1;
}
```

*Informative:* every `set`, `get`, `containsKey` and `remove` on a
`Dict` scans the entries linearly — acceptable for typical small
scripts; no O(1) hashtable lookup is provided.

## 4.12.7 Subscript sugar

`li[i]` on a `List` and `d[k]` on a `Dict` are sugar over the
`get`/`set` methods, and a subscript assignment is sugar over
`set`:

```nlang
List<int> li = {};
li.add(2);
li.add(5);
int v = li[1];          // == li.get(1)   -> 5
li[0] = 9;              // == li.set(0, 9)

Dict<string, int> d = {};
d["a"] = 3;             // == d.set("a", 3)
int x = d["a"];         // == d.get("a")  -> 3
```

Out-of-range reads and writes and missing dict keys raise exactly
as the method forms do (§4.12.6). Because the compiler peels the
element type off the container type, subscripts compose with the
rest of the language:

```nlang
List<List<int>> m = ...;
m[0][1] = 47;           // chained subscript write

List<Point> pts = ...;
pts[0].x = 9;           // member write through a subscript receiver

List<int> arr[2];       // array of instantiations (declaration form)
arr[0] = {};
arr[0].add(1);
arr[0][1];              // container subscript through an array element
```

Compound subscript assignment (`li[0] += 1`) follows the same
policy as arrays (chapter 5, §4.10.5).

## 4.12.8 Generics across compile units

A generic class declared in one compile unit is fully usable from
another: after `import boxlib;`, `Box<int>` names an instantiation
of the imported template, and construction, member calls and type
checking work exactly as within one unit.

```nlang
import boxlib;
int main() {
    Box<int> bi = Box(3);
    return bi.get();
}
```

```nlang boxlib.n
class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}
```

An imported function carries its true formal and return types
recursively — instantiations, array types and nested arrays
included — so a call site in the importing unit checks the same
types as a call in the declaring unit.

*Informative:* all instantiations of one template share a single
compiled row; per-object state lives in the instances. A bound
method reference to a template member works: `func<int> f = b.get;`
(§4.13).
