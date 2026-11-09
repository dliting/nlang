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

The equality operators are legal on type-parameter operands: `==`
and `!=` apply to `T` and `T`, `T` and `null` (both directions), and
the method body reads the instantiation information carried by the
receiver object at runtime, dispatching on the kind the
instantiation was made with — scalars compare by inline value,
`string` by content, structs by field value, classes and arrays by
identity, enums by inline value. The `null` side performs a
reference-domain null test against the resolved kind (the value of a
scalar argument is never `null`). Concatenation and printing convert
by the argument kind the same way: `"v=" + item` and `item + "!"`
in both orders fold the `T` value through its actual kind's string
form (scalar rendering, the string itself, a struct's `<struct>`, a
class's virtual `toString`, an enum's numeric value, an array's
format walk) into the result, and `print` does the same directly.
The other kind-dependent operators — relational and arithmetic —
remain compile-time rejections, reporting `operator '...' cannot be
applied to a type parameter: the operand kind is not known until the
instantiation.` A cast recovers a concrete kind: when the
instantiation is known, `item as string == "a"` compares by content
again. Storing, passing and returning a `T` value are
kind-independent and stay legal.

All of these dispatches happen inside an instance method or
constructor body — the instantiation information they read comes
from the receiver object (`this`). Frames of `static` members have
no receiver, so equality, concatenation, printing and `as` casts on
`T` are rejected in `static` context without exception, reporting
`... requires an instance context.` (§4.12.9).

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
a class type, an interface type, a struct type, an array type, or
another generic instantiation. A `void` type argument is a compile-time error
(§7.4).

A user-declared generic class rejects function type arguments: a
function value is a multi-slot record (target, receiver, form). A
struct type argument is admitted — every boundary (constructor
formals, local binds, field stores, `foreach`) deep-copies the
record through the substituted type, so the field's value slot holds
one private copy. The built-in containers accept struct type
arguments alike (a struct key in a `Dict` compares by field value,
§12.2).

One corner of the struct argument: inside a generic class's own
method bodies, a `T`-to-`T` move between two `T`-typed fields
(`item = other.item;`) transfers the record handle without a copy,
so the two fields then share one record — a write through one shows
through the other. Only the in-body field move aliases; every
boundary deep-copies (§4.7.4).

An array type argument — `List<int[]>` — stores the array values as
raw references; the primitive-boxing rule does not apply to them.
The bracket spelling and the generic spelling are the same type
(§4.10.1), so `List<Array<int>>` and `List<int[]>` are one type
written two ways. Elements pulled out with `get` or subscript keep
their array type,
so `foreach (int[] row in grid)` iterates them directly (chapter
6). `indexOf` and `contains` compare array elements by handle
identity. A jagged type argument (`List<int[][]>`) is rejected like
any jagged declaration (§4.10.1) — and so is the same jagged shape
in generic clothes: `Array<Array<int>>` is `int[][]` and is a
compile-time error wherever the type is written. An unannotated
`Array<T>` is a plain type argument: `List<Array<T>>` inside a
generic class declares a list of arrays of the type parameter.

An instantiation works as an element type in the declaration-site
array form — `List<int> arr[2];` — while the creation form
`List<int>[2]` in an expression is a syntax error (§4.10.2). A type
alias may name an instantiation (§3.4): after `using IL =
List<int>;`, `IL` works as a type and as a construction target.

## 4.12.6 Built-in containers List and Dict

`List<T>` is a growable, ordered, index-addressable collection of
elements of type `T`, and `Dict<K, V>` is an associative array
mapping keys of type `K` to values of type `V`. Both are prelude
names, visible without an import (§3.9.4), and ordinary generic
classes: each instantiation is a distinct type (§4.12.1),
construction follows §4.12.3, and type arguments follow §4.12.5.

The method surface — every method with its signature, effect and
failure behavior, the kind-aware matching of elements and keys, the
insertion order of `Dict` keys, and the `Object` instantiation that
mixes value types — is defined in [chapter 12
Collections](../library/12-collections.md).

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
as the method forms do (§12). Because the compiler peels the
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

*Informative:* a unit also carries a template table — one row per
instantiation the unit registers (canonical spelling and
construction recipe). Linking merges the rows of all units; objects
constructed across units carry their instantiation information as
usual (§4.12.9), and equality, casting and `typeName` behave
identically across units.

*Informative:* all instantiations of one template share a single
compiled row; per-object state lives in the instances. A bound
method reference to a template member works: `func<int> f = b.get;`
(§7.4). Instantiations are not transparent at runtime: every object
constructed from an instantiation carries a piece of *instantiation
information* — the template name and the kind and canonical spelling
of each argument — read by equality dispatch, casts and `typeName`
(§4.12.2, §4.12.9).

## 4.12.9 Runtime type information and type parameters

`typeName` takes a type expression and returns its canonical
spelling as a string. Spellings of concrete types fold at compile
time — `typeName(int)` is `"int"`, `typeName(Box<int>)` is
`"Box<int>"`, `typeName(int[,])` is `"int[,]"` (rank in brackets),
`typeName(List<List<int>>)` is `"List<List<int>>"`. Forms containing
a type parameter resolve at runtime through the receiver's
instantiation information:

```nlang
class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
}
class Hold<T> {
    public string showArg() { return typeName(T); }        // "int" / "Box<int>"
    public string showInner() { return typeName(Box<T>); } // "Box<int>"
    public string showArray() { return typeName(T[]); }    // "int[]" / "Box<int>[]"
}
```

A bare `T` reads the receiver's argument spelling; a `T` nested
inside an argument (`Box<T>`, `T[]`) composes level by level over
the same argument, and an array over a type parameter appends the
element spelling in a bracket pair. Like equality, the runtime forms
of `typeName` require an instance context and are rejected in
`static` members. `typeName` is a reserved word (§2.4).

Constructing an instantiation whose arguments involve the type
parameter inside a method body — `List<T> items = {};` — shares one
construction path with concrete instantiations: the object's
instantiation information is composed at the construction point from
the recipe (`T` taken from the receiver's argument, arrays wrap the
element recipe, nesting recurses), and the object's equality,
casting and `typeName` behavior then matches an instance written
with the concrete type.

An `as` cast whose target is a type parameter — `o as T` —
dispatches on the kind of the receiver's argument into the existing
cast runtimes: boxed scalars and `string` unbox, structs row-check,
classes walk the superclass chain and compare the argument's own
instantiation, arrays go by element type, enums by the integer
value. A mismatched target raises a catchable `InvalidCastException`
whose message names the target spelling; a `null` operand passes
through. Casts distinguish instantiations: an instance of
`Box<int>` does not cast to `Box<string>`, even though both share
the same template. Casts with a known instantiation (`item as
string`) are outside this section and follow the normal cast rules
of §5.

The type position of `catch` does not accept generic instantiations
— including the type parameter itself: `catch (Box<int> e)` is a
compile-time error (`catch on a generic instantiation type ...
cannot tell instantiations apart`). Exception dispatch matches along
template rows and cannot tell apart different instantiations of one
template; accepting the spelling would promise a distinction the
runtime cannot make (§8.3).

*A compensation pattern:* where you need to record different
instantiations by type parameter, use a non-generic helper class
holding a dictionary keyed by spelling, and have the generic class
pass `typeName(T)` as the key from an instance method body:

```nlang
class TypeCounter {
    private static Dict<string, int> counts = {};
    public static void bump(string key) {
        if (counts.containsKey(key)) { counts.set(key, counts.get(key) + 1); }
        else { counts.set(key, 1); }
    }
    public static int count(string key) {
        if (counts.containsKey(key)) { return counts.get(key); }
        return 0;
    }
}
class Holder<T> {
    public void note(T item) {
        TypeCounter.bump(typeName(T));   // one key per instantiation
    }
}
```

The key is the canonical spelling `typeName(T)` produces, so
`Holder<int>` and `Holder<Box<int>>` each take their own slot.
