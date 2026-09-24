# Expressions


### Arithmetic

```nlang
a + b    a - b    a * b    a / b    a % b
```

Integer division truncates toward zero. Division/modulo by zero throws a
runtime error — this includes `float` division by zero, which throws
rather than producing IEEE 754 ±inf/NaN (NLang diverges from C/C++/Java
here).

**Integer overflow** wraps silently in two's complement (C-style):
`INT_MAX + 1 == INT_MIN`. There is no SafeInt-style checking. Lock-in
test: `tests/e2e/int_overflow_wrap.n`.

**Numeric promotion**: arithmetic ops follow symmetric C-style
promotion — both operands are promoted to the wider type before the op:
- `int + int` → int
- `int + float` / `float + int` → float (both operands promoted to float)
- `float + float` → float

So `1 + 2.5 == 2.5 + 1 == 3.5` (symmetric). The result type is the promoted
type; assignment to a narrower type (e.g. `int r = 1.5 + 1;`) implicitly
truncates.

`string + string` (OP_Add only) is concatenation. `string - string` etc.
are compile errors.

### Comparison

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

Returns 1 (true) or 0 (false). String equality compares content. String
relational ordering (`<`, `>`, `<=`, `>=`) uses C `strcmp`-style byte-by-byte
comparison (e.g. `"Z" < "a"` is true because `'Z'` (90) < `'a'` (97)). Because
strings are UTF-8 and UTF-8 byte order equals code point order, ordering is
also correct for non-ASCII text: `"é" > "z"` is true.

**Comparison operands are typed**:

- Mixed string/non-string is a **compile error** (`"a" < 5`, `5 == "a"`).
  The one exception is the null literal: `s == null` / `c == null` compare
  against the null sentinel — identity for class/reference operands; for a
  string operand the null side reads as the empty string (handle 0 is the
  reserved null sentinel; a real empty string has its own object, distinct
  from null by bits). `"" == null` therefore compares equal, and any
  non-empty string compares unequal.
- int/float pairs get the same symmetric promotion as arithmetic:
  `-2 < -1.5` promotes to float and is true; `1 == 1.0` is
  true.
- Class/reference equality (`==`, `!=`) is identity (same heap object).
- Arithmetic/concat with a null operand is a compile error — null has a
  value only through the comparison identity path above.

### Logical

```nlang
a && b   a || b   !a
```

Short-circuit, C-style: `&&` evaluates `b` only when `a` is non-zero;
`||` evaluates `b` only when `a` is zero. The skipped operand produces no
observable effect at all — no calls, no throws. The result is always
`int` `0` or `1` (never the raw operand value). Operands of `&&`, `||`
and `!` must be `int` — the same rule as `if`/`while` conditions
(comparisons already produce `int`).

### Member Access

```nlang
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check).

### Object Creation

```nlang
Node n = new Node();
Node n = new Node(42);
```

Allocates on the heap, calls constructor if present.

### Type Casts

```nlang
int x = 5;
float y = (float)x;
int z = (int)y;
```

Explicit casts between int and float. Implicit widening (int→float) is
allowed in some contexts.

### Primitive → String Coercion

When a primitive (int or float) appears in a context expecting string,
NLang auto-coerces it to its decimal string form. This is most common in
string concatenation, but also fires in direct assignment and field stores.

```nlang
string s1 = "x" + 5;       // "x5" — int coerced to "5"
string s2 = 5 + "x";       // "5x" — symmetric
string s3 = "x=" + 2.5;    // "x=2.5" — float uses %g format
string s4 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
string s5 = 42;            // "42" — direct assignment path
string s6 = "x" + (-7);    // "x-7" — negative formatted with sign
```

**Implementation**:
- `int → string`: `OP_Int32_to_str` (decimal, via `std::to_string`)
- `float → string`: `OP_Float_to_str` (`%g` format — `2.5` not `2.500000`)
- Both mint the formatted string as a runtime string object and write
  the new handle into the result slot; `OP_Assign` then moves the result
  into the destination slot.
- `string → int/float` remains rejected — use
  the standard library's `s.toInt()` / `s.toFloat()` instead (see Standard
  Library).

**Object.toString() Protocol**:

All class instances inherit `string toString()` from `Object`. The default
implementation returns `"ClassName@heapIdxHex"` (e.g. `"Point@7"`, `"Point@ff"`).
User classes override it by declaring `string toString() { ... }` — virtual
dispatch by name, same as `equals`/`getHashCode`.

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

Dispatch matrix:

| Receiver | `.toString()` result | Override? |
|----------|---------------------|-----------|
| class (user override) | user-defined | yes |
| class (no override) | `"ClassName@hex(heapIdx)"` | no (Object intrinsic) |
| enum | enum member name (e.g. `"Red"`) | no |
| int | decimal string (e.g. `"42"`) | no |
| float | `%g` format (e.g. `"2.5"`) | no |
| string | self (identity) | no |

**Implicit coercion**: `"x" + obj` automatically calls `obj.toString()`, same
as Java/C#. This applies to class, enum, int, and float receivers. Struct
receivers are **permanently excluded** — `"x" + structInstance` is a compile
error (struct is a pure-data type in NLang; use class for object semantics).

**Enum name output**: `Color.Red.toString()` returns `"Red"` (not `"0"`). The
compiler embeds a per-enum name table; the VM uses `OP_Enum_to_str` to look up
the member name by value. Out-of-range enum values throw at runtime.

**String identity**: `"hello".toString()` returns `"hello"` — the compiler folds
this to a no-op (no opcode emitted).

**Limitations**:
- No warning when implicit coercion occurs (silent, like Java)
- `struct.toString()` / `"x" + structInstance` — permanently rejected

**Escape sequences** (inside double-quoted literals):

| Escape      | Produces              |
|-------------|-----------------------|
| `\n` `\r` `\t` | newline, CR, tab   |
| `\\` `\"` `\'` | backslash, quote, apostrophe |
| `\0` `\a` `\b` `\f` `\v` | NUL, bell, backspace, form feed, vertical tab |
| `\x`/`\u`... | not supported        |

Any other escape (e.g. `\q`) is a compile error — escapes never pass
through as literal backslash pairs. Interpolation and escapes compose:
`"${name}\n"` interpolates then appends a newline.


### String Interpolation

```nlang
string name = "world";
string s = "Hello ${name}!";   // "Hello world!"
```

NLang supports `${identifier}` interpolation inside double-quoted string
literals — the named variable's value is rendered via the same coercion
paths as primitive → string and collection
`toString()`. Interpolation is rewritten at compile time into an
equivalent `OP_Add` string-concatenation expression; no new opcode is
introduced.

**Syntax constraints**:

- Only a single identifier is supported inside `${...}`. Complex
  expressions like `${a + b}`, `${obj.method()}`, or `${this.x}` are
  rejected at parse time. Use a separate variable: `int sum = a + b;
  "result=${sum}"`.
- `${name}` where `name` is not in scope produces a compile error ("undefined
  identifier") — the same path as any other undefined
  identifier reference.
- Empty `${}` and invalid identifier contents (e.g. `${123}`, `${a b}`)
  produce a compile error.

**Dollar escape**: `$$` produces a literal `$` in the resulting string.
`$${name}` produces the literal text `${name}` (no interpolation). A lone
`$` not followed by `$` or `{` is preserved as a literal `$`.

```nlang
string name = "x";
string a = "$${name}";  // literal "${name}"
string b = "price: $";  // literal "price: $"
string c = "$$100";     // literal "$100"
```

**Type dispatch**: the identifier's resolved type determines the coercion
applied automatically:

| Identifier type | Coercion applied |
|-----------------|------------------|
| `int` | `OP_Int32_to_str` |
| `float` | `OP_Float_to_str` |
| `string` | none |
| `enum` | `OP_Enum_to_str` |
| `Array` | `OP_Array_to_str` |
| `List` / `Dict` | `OP_CallMethod "toString"` |
| `class` | `OP_CallMethod "toString"` |

### Runtime-checked Cast (`as`)

```nlang
expr as TypeName
```

The following runtime-checked conversions are supported:

- **Unbox**: `o as int` / `o as float` / `o as string` — unwrap a boxed
  primitive. Throws if `o` is null or the boxed type tag doesn't match.
- **Class downcast**: `o as SubClass` — verify the runtime class of `o`
  is `SubClass` or a subclass thereof. Throws on mismatch.
- **Identity / upcast**: `o as Object` — no-op (any class is already
  Object). Allowed for symmetry.

Type-incompatible casts (`5 as string`, `o as int` when `o` holds a
class ref) are compile errors — `as` only permits same/box/unbox/downcast.
Array operands are rejected outright (`ia as int`, `ia as Object` —
"the cast operand is an array"): an array value converts only through
assignment (to its own array type, plus the whole-value string
coercion), never through `as` (see
[Known Limitations](known-limitations.md) for the full array-value
conversion rule).

### Collection Initializers

NLang supports C-style collection literals for arrays, lists, dicts,
and aggregate (struct/class) initialization. Two syntactic forms:

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
(function args, return values, standalone expressions). Required for
dict, struct, and class initialization because bare `{...}` would
conflict with the block-statement grammar (a `{...}`-surrounded
statement group):

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

**Type disambiguation:** the compiler uses the LHS variable (or the
explicit `Type` in `new Type{...}`) to pick the kind:

| Target type          | Form    | Entry kind              |
|----------------------|---------|-------------------------|
| `T[]` (array)        | `[...]` | value-only              |
| `List<T>`            | `[...]` or `new List<T>{...}` | value-only |
| `Dict<K,V>`          | `new Dict<K,V>{...}` | `key : value` (string key) |
| struct               | `new StructName{...}` | `field : value` (identifier key) |
| class                | `new ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `new C{f1:v1, ...}` as
`new C()` followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists.
Nested generic element types (`List<List<int>>`, `Dict<K, List<V>>`)
are supported (see Nested generics under
`List<T>` above).

**Empty collections:** bare `[]` is not supported (the lexer matches
`[]` as a single token, used by the array-type suffix grammar). Use
the explicit empty form instead: `new List<T>{}`, `new Dict<K,V>{}`,
or `new int[0]` for arrays.

**Function arg disambiguation:** a bare `[...]` as a function argument
is not currently supported — it produces a compile error because the
compiler cannot infer the target type without overload resolution.
Use the explicit `new Type{...}` form for function args.

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from
within an entry expression (e.g. `[1, foo(arr)]` where `foo` reads
`arr`) is undefined behavior. An exception-throwing entry leaves the
collection partially constructed.
