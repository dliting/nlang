# Common Error Messages

The NLang compiler's error messages, grouped by error family. Each
family shows the trigger shape, the error text, a one-line cause, and a
pointer to the full rule. Systematic limits (what you *can* do, rather
than how it errors) are on [Known Limitations](known-limitations.md);
quick answers to high-frequency questions are on the [FAQ](../getting-started/faq.md).

### Array values (scalar contexts)

```nlang
int main() {
    int[] arr = [1, 2, 3];
    int x = arr;
    return 0;
}
```

```text
Error: Incompatible type "arr".
```

An array value has only two legal destinations — its own array type and
every `string` target (`toString`, etc.); every other scalar context is
rejected with an explicit error. See → [Known Limitations](known-limitations.md), the
"Array values in scalar contexts" item.

### Conditions and logic

```nlang
int main() {
    string s = "x";
    if (s) { }
    return 0;
}
```

```text
Error: if condition must be bool, not "String".
```

The condition of `if`/`while`/`do-while`/`for`/`assert` and the operands
of `&&`/`||`/`!` must all be `bool` (comparisons and predicates already
produce bool) — int, string, float, char, class, struct, and array are
all rejected with explicit errors (`if condition must be bool, not "Int32"`,
`operator '&&' requires bool operands, got "Int32"`). Rewrite as an
explicit comparison: `if (s != "")`, `if (count != 0)`. See →
[Statements](statements.md) "Condition typing" and
[Operators](operators.md) "Logical".

### import and visibility

```nlang
import nosuch;
```

```text
Error: Module 'nosuch' not found. Check the project Sources list or -I import path.
```

`import` only opens qualified names. Referencing a member of an
unimported standard library package (or a known project module) without
importing it reports a "not imported" diagnostic — for example, calling
`io.print` without `import io;` reports `Package 'io' is not imported.
Add 'import io;' at the top of this file.`. See →
[Declarations](declarations.md) "Import Declaration".

### Default expressions on imported functions

```nlang
// lib.n
int helper() { return 1; }
int g(int a = helper() as int) { return a; }

// main.n
import lib;
int main() {
    return lib.g();
}
```

```text
Error: imported function 'lib.g' has a default for parameter 0 outside the cross-module subset; supported: int/float/double/long/ulong/string/null literals, formal parameter references, unary negation, calls to the exporting module's free functions, and + - * / % arithmetic over those
```

A default expression crosses the `.ncu` boundary only inside the
serialized subset: literals (including string and null), references to
earlier formal parameters, calls to the producing module's own free
functions, unary negation, and `+` `-` `*` `/` `%` arithmetic over
those — still evaluated at each call site. Casts, member access, and
calls nested as arguments sit outside the subset; importing the
function rejects them on the consumer side with the error above.
See → [Default Parameters](default-parameters.md).

### Declarations and control flow

```nlang
int main() {
    if (1 == 1)
        int a = 2;
    return 0;
}
```

```text
Error: a local declaration cannot be the unbraced body of a control-flow statement; use braces
```

A control-flow statement's unbraced (bare) body cannot be a declaration
— use a braced body. When calling a function with an `out` parameter,
the call site must carry the `out` marker, otherwise you get `The
function invoke "foo(y)" is not compatible with the declaration.`. See
→ [Statements](statements.md) "Unbraced body" and
[Out Parameters](out-parameters.md).

### Assignment targets

```nlang
class Counter {
    public int n;
    public int step() { n = n + 1; return n; }
}

int main() {
    Counter c = Counter();
    c.step() += 1;
    return c.n;
}
```

```text
Error: cannot assign to the result of a method call.
```

A method call yields a value, not a location — both `c.step() = v` and
`c.step() += v` are rejected with this error. To write a field, use `c.n`
as the left value; the full set of supported left values is in
[Compound Assignment](compound-assignment.md).

### The method "f" claims the virtual dispatch slot of ...

```nlang
class A {
    public virtual int f(int x) { return x; }
}
class B : A {
    public int f(float x) { return 1; }
}

int main() {
    return 0;
}
```

```text
Error: The method "f" claims the virtual dispatch slot of "A.f(Int32): Int32" with a different signature "f(Float): Int32". Align the signature to override, or rename the method.
	See also the declaration of "A.f(Int32): Int32".
Error: The class "B" does not implement the method "f(Int32): Int32" required by its inheritance contract.
```

A subclass method shares a name with an ancestor virtual method or an
interface contract but has a different signature. Virtual dispatch works by
name, so a signature mismatch reads arguments as wrong bits. The
mismatching declaration reports the primary message (its final sentence is
the remediation hint) plus a `See also the declaration of ...` line
pointing at the contract, and the class declaration adds a
`required by its inheritance contract` line; when the contract is an
interface method the anchor reads `"IF.val(Int32): Int32"` (next entry).
Make the signature match the contract exactly (an override), or rename the
method. See → [Class](class.md), "Virtual methods and overrides".

### The method "f" claims the virtual dispatch slot of ... with a different signature ... Virtual dispatch is name-based

```nlang
class A {
    public virtual int f(int x) { return 1; }
    public int f(string s) { return 2; }
}

int main() {
    return 0;
}
```

```text
Error: The method "f" claims the virtual dispatch slot of "A.f(Int32): Int32" with a different signature "f(String): Int32". Virtual dispatch is name-based — rename the overloads, or make the family non-virtual.
	See also the declaration of "A.f(Int32): Int32".
```

Two overloads of one method name in the **same class body**, where at
least one is `virtual`. Virtual dispatch resolves by method name at
runtime, so one name owns one dispatch slot per class; a second
signature on that name could only ever run with bit-wrong argument
reads. Rename the overloads so each signature gets its own name, or
drop `virtual` from the whole family — fully non-virtual families
overload normally. The rendered signatures use the internal type names
(`Int32`, `String`, `Float`), and a `static` member mixed with an
instance `virtual` member is rejected the same way (`static` counts as
a signature difference). See → [Class](class.md), "Virtual methods and
overrides".

### The class "F" does not implement the method "val(Int32): Int32" required by interface "IF"

```nlang
interface IF {
    public int val(int x);
}
class F implements IF {
    public int other() { return 0; }
}

int main() {
    return 0;
}
```

```text
Error: The class "F" does not implement the method "val(Int32): Int32" required by interface "IF".
```

```nlang
interface IF {
    public int val(int x);
}
class F implements IF {
    public int val(string s) { return 1; }
}

int main() {
    return 0;
}
```

```text
Error: The method "val" claims the virtual dispatch slot of "IF.val(Int32): Int32" with a different signature "val(String): Int32". Align the signature to override, or rename the method.
	See also the declaration of "IF.val(Int32): Int32".
Error: The class "F" does not implement the method "val(Int32): Int32" required by its inheritance contract.
```

An interface method that a class declares it implements via `implements`
is missing or mismatched. When missing, the class declaration reports the
interface-anchored message; when an implementation exists but its
signature differs, the `claims the virtual dispatch slot` family fires
(as in the previous entry, with the interface method as the anchor); if
the mismatching implementation is inherited from a parent (not redeclared
in the class), the variant
`The class "C" does not implement the method "val(Int32): Int32" required by interface "IF" (found "val(String): Int32" with a different signature).`
is reported instead. Provide an implementation with the exact signature
in the class (or one of its parents); note that a same-name method must
not coexist as an overload of an interface contract. See →
[Interface](interface.md), "Implementation contract".

### switch and enum

```nlang
int main() {
    List<int> xs = {};
    switch (xs) {
        case 1:
            return 1;
        default:
            return 2;
    }
}
```

```text
Error: switch discriminant must be int, float, string, or enum
```

A switch discriminant may be an integer-family value (including char),
`float`/`double`, `string`, or enum; bool, class, struct, `List`,
`Dict`, and array values are rejected — use `if`/`else` + `equals()`
for class values, and expand bool discrimination into `if`/`else`. See
→ [Switch](switch.md).

### Literal and constant fit

```nlang
int main() {
    byte b = 1000;
    return 0;
}
```

```text
Error: constant 1000 out of range for 'byte'
```

```nlang
int main() {
    int x = 1e30;
    return 0;
}
```

```text
Error: Incompatible type "1e+30".
```

```nlang
int main() {
    string s = "${123}";
    return 0;
}
```

```text
Error: invalid identifier "123" in ${...}: only ${name} supported (no expressions).
```

An integer **literal** assigned to a narrower target must be within the
target's range (a constant-fit special case — `byte b = 5` is legal);
unsuffixed decimal and exponent forms are `double` literals, and integer
targets never accept float constants (`int x = 2e5` likewise reports
Incompatible type); string interpolation only supports `${name}` named
variables, not expressions. See → [Primitives](primitives.md) "Numeric
literals" and [String](string.md) "String interpolation".

### Numeric type mixing

```nlang
int main() {
    int i = 1;
    ulong u = 2;
    long x = i + u;
    return 0;
}
```

```text
Error: no implicit common type for "Int32" and "ULong".
```

```nlang
int main() {
    char c = 'a';
    bool z = c < 100;
    return 0;
}
```

```text
Error: a char value can only be compared with a char value.
```

```nlang
int main() {
    long l = 5000000000;
    float f = l;
    return 0;
}
```

```text
Warning: implicit conversion from 'Long' to 'Float' loses precision
```

Arithmetic mixing `int`/`long` with `ulong` has no implicit common type
— unify the sign domain explicitly first (`i as ulong` or `u as long`);
char never converts implicitly to a numeric type (take the code point
with `c as int` before comparing, likewise for arithmetic); implicit
assignment of int/uint/long/ulong→float and long/ulong→double raises a
**lossy warning** (it does not affect the exit code; `ncc --no-warn`
suppresses all warnings; an explicit `as` never warns). See →
[Type Semantics](type-semantics.md) "Numeric conversion matrix" and
[Operators](operators.md) "Arithmetic".

### `as` casts

```nlang
int main() {
    ubyte b = 200;
    int i = b as int;
    return 0;
}
```

```text
Error: `as` cannot perform implicit conversion `UByte` → `Int32`.
```

```nlang
int main() {
    string s = "5";
    int x = s as int;
    return 0;
}
```

```text
Error: Invalid cast: `String as Int32` is not allowed.
```

`as` exists only for pairs the conversion matrix does not admit —
writing `as` for a widening that is already implicitly legal is
redundant (`ubyte→int`: just `int i = b;`); `as` from string to a
numeric type is forbidden — use the `s.toInt()`/`toLong()`/
`toDouble()` method family. See → [Type Casts](type-casts.md).

### Dict initializer keys

```nlang
int main() {
    Dict<int, int> d = {"1": 2};
    return 0;
}
```

```text
Error: the Dict collection initializer requires string keys; use set() with an explicit 'Int32' key
```

The collection initializer emits dict keys as string constants and boxes
them by K's tag — this is only correct for `K = string`. Construct a
non-string-keyed `Dict<K,V>` with the empty initializer, then fill
entries with `set()`. See →
[Collection Initializers](collection-initializers.md).

### A struct without a constructor cannot take arguments

```nlang
struct Point { int x; int y; }
int main() {
    Point p = Point(2, 3);
    return 0;
}
```

```text
Error: struct "Point" has no constructor; Point() cannot take arguments
```

A struct that declares no constructor can only be created with a bare
declaration (`Point p;`) or a brace entry list (`Point{x: 2, y: 3}` /
`Point{2, 3}`) — it cannot be called with arguments. Declare a constructor
to call it: `public Point(int a, int b) { x = a; y = b; }` makes `Point(2, 3)`
run it (see [Struct](struct.md) "Creating and initializing" and
[Collection Initializers](collection-initializers.md)).

### Generic braced anchors

```nlang
int main() {
    List<int> nums = List<int>{60};
    return 0;
}
```

```text
Error: syntax error
Error: Invalid statement.
```

An anchored head on a generic type (`List<int>{...}`) does not parse.
Give the literal a typed declaration instead: `List<int> nums = [60];`.
See → [Known limitations](known-limitations.md), "Generic instantiation
has no anchored head", and [Collection
Initializers](collection-initializers.md).

### Type aliases

```nlang
using A = int;
using A = float;
```

```text
Error: The type alias "A" is defined more than once in this translation unit.
```

The same alias name may not be defined twice in one translation unit; a
circular alias (`using A = B; using B = A;`) is rejected. See →
[Declarations](declarations.md) "Type Aliases".

### Source file encoding

```nlang
int main() {
    string s = "中文内容";   // the editor saved the file as GBK/ANSI
    return 0;
}
```

```text
Error: Source file src_not_utf8.n is not valid UTF-8 (first invalid byte at line 2). Save the file as UTF-8.
```

`.n` sources and `.nproj` project files pass strict Unicode Transformation Format (UTF-8) validation
before tokenizing (the enforcement point is ncc; nide's build invokes
ncc, so building there is subject to the same validation): invalid bytes are
rejected with an explicit error (the
line of the first invalid byte included) and a UTF-16 save gets a
dedicated hint (`Source file ... is UTF-16, not UTF-8.`) — legacy
encoding bytes never reach string constants. A
leading UTF-8 byte order mark (BOM) is accepted and skipped. See →
[Primitives](primitives.md) "Representation: from source to console".
