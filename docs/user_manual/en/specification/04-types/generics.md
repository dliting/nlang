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
name. A declaration declares at most 255 type parameters — the
runtime carries a type parameter's ordinal in one byte and reserves
the top value as a marker, so the last usable ordinal is 254; the
limit covers generic free functions too (§4.12.10).

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
`0` for an integer argument, `""` for a `string` argument, `null`
for a class argument.

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

All of these dispatches happen inside a method or constructor body —
an instance body reads the instantiation information from the
receiver object (`this`). A `static` body has no receiver; its
instantiation context comes from the call site instead. A call
through the bare class name infers the type arguments from the
actuals — every type parameter needs a supplying actual — and a call
through an explicit instantiation spelling fixes them outright:
`Pair<int, string>.sig()`. With the context in place, equality,
concatenation, printing, `as` casts and `typeName` on `T` work in
`static` bodies exactly as in instance bodies (§4.12.9).

```nlang
class Pair<K, V> {
    public static string sig() {
        return typeName(K) + "," + typeName(V);
    }
    public static K firstOf(K k, V v) {
        return k;
    }
}
int main() {
    if (Pair<int, string>.sig() == "int,string"
            && Pair.firstOf(4, "x") == 4) {   // firstOf infers K, V from the actuals
        return 2;
    }
    return 1;
}
```

A call that leaves a type parameter without a supplying actual is a
compile-time error naming the fix (`cannot infer type argument for
"K, V" in "Pair.eq(...)"; spell the instantiation: Pair<K, V>(...)`).
A function of a generic class cannot be referenced as a `func` value
through either spelling — the handle record has no slot for the
instantiation context; call it directly.

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

The explicit instantiation spelling constructs without a declared
target — the spelled type arguments fix the instantiation at the
call: `Box<int> b = Box<int>(7);`. Both forms share one construction
path; the constructed object carries its instantiation information
the same way (§4.12.9).

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

*Informative:* instantiations nest at most 16 levels deep;
exceeding the limit reports `The generic instantiation depth
exceeds the limit of 16 nested instantiations.`

## 4.12.5 Type arguments

A type argument may be a scalar primitive, `string`, an enumeration,
a class type, an interface type, a struct type, an array type, or
another generic instantiation. A `void` type argument is a compile-time error
(§7.4).

A user-declared generic class rejects function type arguments: a
function value is a multi-slot record (target, receiver, form). A
struct type argument is admitted — constructor formals, local binds,
and `foreach` loop variables each establish a fresh record copied
from the source, while field stores and field-to-field moves update
records in place per §4.7.4, so the field's value slot holds one
private copy. The built-in containers accept struct type
arguments alike (a struct key in a `Dict` compares by field value,
§12.2).

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
types as a call in the declaring unit. A generic free function is
fully usable across units as well: the imported signature carries
its type parameters, and inference, forwarding and `typeName`
dispatch at the consumer's call sites work exactly as in the
declaring unit (§4.12.10).

*Informative:* a unit also carries a template table — one row per
instantiation the unit registers (canonical name and
composition record). Linking merges the rows of all units; objects
constructed across units carry their instantiation information as
usual (§4.12.9), and equality, casting and `typeName` behave
identically across units.

*Informative:* all instantiations of one template share a single
compiled row; per-object state lives in the instances. A bound
method reference to a template member works: `func<int> f = b.get;`
(§7.4). Instantiations are not transparent at runtime: every object
constructed from an instantiation carries a piece of *instantiation
information* — the template name and the kind and canonical name
of each argument — read by equality dispatch, casts and `typeName`
(§4.12.2, §4.12.9).

## 4.12.9 Runtime type information and type parameters

`typeName` takes a type expression and returns its canonical
spelling as a string. Spellings of concrete types fold at compile
time — `typeName(int)` is `"int"`, `typeName(int[,])` is
`"int[,]"` (rank in brackets), and the built-in containers keep
their bare names: `typeName(List<List<int>>)` is
`"List<List<int>>"`. A user unit's generic template spells with
its owning package path prefixed (§3.9.3): in a unit compiled
from `demo.n`, `typeName(Box<int>)` is `"demo.Box<int>"`, and the
same-named template imported from package `alib` spells
`"alib.Box<int>"` — two packages declaring a same-named template
produce two distinct spellings, across `typeName`, casts and
equality alike. Forms containing
a type parameter resolve at runtime through the receiver's
instantiation information:

```nlang
class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
}
class Hold<T> {
    public string showArg() { return typeName(T); }        // "int" / "demo.Box<int>"
    public string showInner() { return typeName(Box<T>); } // "demo.Box<int>"
    public string showArray() { return typeName(T[]); }    // "int[]" / "demo.Box<int>[]"
}
```

A bare `T` reads the receiver's argument spelling; a `T` nested
inside an argument (`Box<T>`, `T[]`) composes level by level over
the same argument, and an array over a type parameter appends the
element spelling in a bracket pair. In a `static` body the runtime
forms read the call site's instantiation context — filled by the
bare-name inference or the explicit spelling of the call (§4.12.2) —
so `typeName(T)` works there exactly as in instance bodies.
`typeName` is a reserved word (§2.4).

Constructing an instantiation whose arguments involve the type
parameter inside a method body — `List<T> items = {};` — shares one
construction path with concrete instantiations: the object's
instantiation information is composed at the construction point from
the record (`T` taken from the receiver's argument, arrays wrap the
element composition record, nesting recurses), and the object's equality,
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

*A compensation pattern:* a generic class cannot declare static
fields — its members belong to the one erased implementation every
instantiation shares — so per-instantiation state that must outlive
instances lives in a non-generic helper class holding a dictionary
keyed by spelling, with the generic class passing `typeName(T)` as
the key. Behavior that only reads the instantiation needs no helper:
`static` methods dispatch on the call site's context (§4.12.2), and
instance methods on the receiver:

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

The key is the canonical name `typeName(T)` produces, so
`Holder<int>` and `Holder<Box<int>>` each take their own slot.

## 4.12.10 Generic free functions

A top-level function may be generic too: type parameters are
declared in angle brackets after the function name and name ordinary
types everywhere in the signature and body — parameter, return and
local types. The body compiles once and is shared by every call,
like a generic class's members.

```nlang
T pick<T>(T a, T b) { return a; }
string tag<T>(T v) { return typeName(T); }

int main() {
    if (pick(3, 7) == 3                     // T = int
            && pick("a", "b") == "a"        // T = string
            && tag(9) == "int") {           // typeName reads the call site
        return 1;
    }
    return 0;
}
```

Calls carry no type-argument spelling — `pick<int>(3, 7)` is not the
syntax. Every type parameter is inferred from the call's actuals,
and the inference is exact match: two actuals supplying one type
parameter with different kinds do not reconcile (`conflicting type
arguments for T: "Int32" vs "String" (inference is exact match; pass
arguments of one type).`). Any type an argument can have can supply
a parameter — scalar primitives, `string`, enums, classes,
interfaces, structs, array types and nested instantiations: with
both `rows` and `cols` declared `int[]`, `pick(rows, cols)` infers
`T = int[]`.

A type parameter that only the return type mentions infers from the
call's expected type: with `List<T> makeList<T>()`, the call
`List<int> xs = makeList();` binds `T = int` from the assignment
target. A result discarded outright leaves no inference source — the
call reports `cannot infer type argument for T: T appears only in
the return type; assign the result to a typed target.` A formal
whose type is a bare type parameter carries no default value, as in
§4.12.2.

Inside the body, type parameters behave exactly as in a `static`
member body (§4.12.2): the instantiation context comes from the call
site, and equality, concatenation, printing, `as` casts and
`typeName` on `T` dispatch on the kind the call was inferred with.
The compile-time rejections are the same — relational and arithmetic
operators on `T` report the type-parameter error, while storing,
passing and returning a `T` value stay legal.

A generic function may call other generic functions, forwarding its
own type parameters; recursion is allowed, and a `T`-typed result
composes as an actual of an outer inference:

```nlang
T head<T>(T a, T b) { return a; }
T twice<T>(T v) { return head(v, v); }    // twice forwards T to head
string label<T>(T v) { return typeName(T); }
// label(twice(9)) == "int": the inner call infers first, the outer
// composes over its result
```

A generic free function cannot be referenced as a `func` value — the
handle record has no slot for the instantiation context, like a
generic class's functions (§4.12.2); call it directly. Across
compile units, an imported generic free function is fully usable
from the importing unit: inference, forwarding and `typeName`
dispatch work through the imported signature exactly as within one
unit (§4.12.8).

## 4.12.11 Generic interfaces

An interface may declare type parameters the way a generic class
does (§4.12.2): the parameters are spelled in angle brackets after
the interface name and name ordinary types in member signatures.
Instance members stay abstract signatures (§4.9.2) and may use the
type parameters in parameter and return types; static methods carry
bodies and compile once against the parameters, like a generic
class's members.

```nlang
interface Pair<T, U> {
    public T first(T a, U b);
    public U second(T a, U b);
    public static T echo(T v) { return v; }
}
```

A class implements a generic interface with an instantiation: the
type arguments are spelled at the `implements` clause and fix the
contract the class must satisfy — every instance signature is
checked against the substituted types.

```nlang
class Both implements Pair<int, string> {
    public int first(int a, string b) { return a; }
    public string second(int a, string b) { return b; }
}
```

Each instantiation is a distinct interface type (§4.12.1): one
class may implement `Pair<int, string>` and `Pair<string, int>`
side by side, and a value of one instantiation does not convert to
another. The argument count must match the declaration — spelling
`implements Pair<int, string>` against `interface Pair<T>` reports
`The generic interface "Pair" expects 1 type argument(s).` A class
that omits a member reports the ordinary contract diagnostic with
the substituted signature (§4.9.1).

Values and dispatch follow the ordinary interface model: an
instance call through an interface-typed value dispatches by name
into the implementing class's records (§4.9.4), and `T`-typed
positions move values by the kinds the instantiation fixed —
scalar arguments store through the reference model exactly as
container elements do (§4.12.1).

A static member is called through an instantiation spelling, which
fixes the instantiation at the call site the way a generic class's
static call does (§4.12.2):

```nlang
Pair<int, string>.echo(7)     // T = int,    returns 7
Pair<string, int>.echo("z")   // T = string, returns "z"
```

The static face keeps its ordinary rules: constants take concrete
types only — a constant cannot have a type parameter as its type,
because no value domain exists before the instantiation — and a
`T`-typed formal carries no default value (§4.12.2). Across
compile units the imported template is usable exactly as within
one unit (§4.12.8): after `import lib;`, `Pipe<int>.echo(21)`
calls the imported interface's static with the call site's
instantiation.
