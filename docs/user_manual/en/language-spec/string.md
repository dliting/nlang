# String


`string` is a primitive, but unlike the scalar types (integers, floats,
`bool`, `char`) it is an **immutable object** referenced by a handle. This
page covers the string type end to end: what it is, its memory semantics,
how to build and compare them, and escape sequences and interpolation
inside literals.

### Value semantics

String is a **value type** in the type system: assignment, parameter
passing, and return copy the handle, and `==` compares **content**, not
identity. The object behind the handle is immutable, so sharing a handle is
harmless — two names to the same string can never diverge. (Contrast with
`class`, a reference type: see [Class](class.md) and
[Type Semantics](type-semantics.md).)

### Encoding and length

String literals are stored as their Unicode Transformation Format (UTF-8) byte sequence in the module string
constant table. `string.length()` returns the **byte count**, not the Unicode
code-point count — `"héllo".length()` is 6 (5 code points, but `é` is 2 bytes
in UTF-8); the code-point count is `charCount()`. Byte access and code-point
access are two different paths over the same string — see "The char bridge"
below. `io.print` writes the raw bytes verbatim to standard output, so the
console sees UTF-8; the full representation chain (source file, compiled
char, console) is on [Primitives](primitives.md) "Representation: from
source to console".

### Memory semantics

Strings are **immutable objects** at run time: concatenation and substring
extraction produce new content, and an existing object's content never
changes. A string slot holds a handle into the string object store; handle 0
means null and **reads as the empty string** `""`.

- **Constants live as long as the execution**: string literals from the
  source are materialized as immortal objects when the module starts
  executing and are never collected during the run.
- **Short-string interning**: runtime strings up to 40 bytes are interned by
  content — identical content shares one object. `==` still compares content;
  the semantics are unchanged.
- **O(1) appends**: `s = s + x` first records a concatenation node and
  flattens it into real content on first read, so appending in a loop never
  copies the whole string step by step.
- **Bounded memory**: unreferenced string objects are reclaimed by the
  garbage collector, so long-running programs (prompt-building loops and
  similar) do not grow memory without bound.

### Building strings

- **Concatenation**: `string + string` and `string + scalar primitive` (the
  scalar is coerced — see [Type Casts](type-casts.md) "Primitive → String
  Coercion"; a char concatenates as its UTF-8 encoding, so `"x" + 'y'` is
  `"xy"`). `string - string` and the other arithmetic forms are compile
  errors.
- **Interpolation**: `${identifier}` inside a literal — see below.
- **Escape sequences**: see below.
- **Explicit construction from a primitive**: a scalar primitive's
  `.toString()` / the primitive → string coercion — see
  [Type Casts](type-casts.md).

### Comparison

`==` / `!=` compare **content**. Relational ordering (`<`, `>`, `<=`, `>=`)
uses C `strcmp`-style byte-by-byte comparison (e.g. `"Z" < "a"` is true because
`'Z'` (90) < `'a'` (97)). Because strings are UTF-8 and UTF-8 byte order
equals code-point order, ordering is also correct for non-American Standard Code for Information Interchange (ASCII) text:
`"é" > "z"` is true. Mixed string/non-string comparison is a compile error
except against the null literal — `s == null` reads the null side as the empty
string, so `"" == null` is true and any non-empty string compares unequal. The
full comparison rules are on [Operators](operators.md) "Comparison".

### Escape sequences

Inside double-quoted literals:

| Escape          | Produces                                              |
|-----------------|------------------------------------------------------|
| `\n` `\r` `\t`  | newline, carriage return (CR), tab                   |
| `\\` `\"` `\'`  | backslash, double quote, apostrophe                   |
| `\0` `\a` `\b` `\f` `\v` | NUL, bell, backspace, form feed, vertical tab |
| `\uXXXX`          | the UTF-8 encoding of that code point (4 hex digits) |

`\uXXXX` covers 4-hex-digit Basic Multilingual Plane (BMP) code points only. **Adjacent surrogate-range
escapes combine into one code point**: `"\ud83d\ude00"` is U+1F600 😀
(as in Java); a surrogate-range escape appearing alone is a compile error.
char literals support the same `\uXXXX` form but **never accept the
surrogate range** (char excludes surrogate code points — see
[Primitives](primitives.md)).

Any other escape (e.g. `\q`, `\x`) is a compile error — escapes never pass
through as literal backslash pairs. Escapes compose with interpolation:
`"${name}\n"` interpolates then appends a newline.

### String interpolation

```nlang
string name = "world";
string s = "Hello ${name}!";   // "Hello world!"
```

NLang supports `${identifier}` interpolation inside double-quoted string
literals — the named variable's value is rendered via the same coercion paths
as primitive → string and collection `toString()` (see [Type Casts](type-casts.md)).
Interpolation is rewritten at compile time into an equivalent `OP_Add`
string-concatenation expression; no new opcode is introduced.

**Syntax constraints**:

- Only a single identifier is supported inside `${...}`. Complex expressions
  like `${a + b}`, `${obj.method()}`, or `${this.x}` are rejected at parse
  time. Use a separate variable: `int sum = a + b; "result=${sum}"`.
- `${name}` where `name` is not in scope produces a compile error ("undefined
  identifier") — the same path as any other undefined identifier reference.
- Empty `${}` and invalid identifier contents (e.g. `${123}`, `${a b}`) produce
  a compile error.

**Dollar escape**: `$$` produces a literal `$` in the resulting string.
`$${name}` produces the literal text `${name}` (no interpolation). A lone `$`
not followed by `$` or `{` is preserved as a literal `$`.

```nlang
string name = "x";
string a = "$${name}";  // literal "${name}"
string b = "price: $";  // literal "price: $"
string c = "$$100";     // literal "$100"
```

**Type dispatch**: the identifier's resolved type determines the coercion
applied automatically:

| Identifier type | Coercion applied       |
|-----------------|------------------------|
| scalar primitives (integer family, `float`/`double`, `bool`, `char`) | `OP_Prim_to_str <kind>` |
| `string`          | none                   |
| `enum`            | `OP_Enum_to_str`       |
| `Array`           | `OP_Array_to_str`      |
| `List` / `Dict`   | `OP_CallMethod "toString"` |
| `class`           | `OP_CallMethod "toString"` |

### The char bridge

A string is a UTF-8 **byte string**; a char is one Unicode scalar value
(see [Primitives](primitives.md)). The bridge between them:

- **Byte access**: `s[i]` returns byte i as a `ubyte` (out of range
  throws the runtime error `string index out of range`).
- **Code-point iteration**: `foreach (char c in s)` binds each complete
  code point to `c` (UTF-8 decoding advances — multi-byte characters are
  never split) — see [Foreach](foreach.md).
- **Code-point access**: `s.charAt(i)` returns the code point starting at
  byte i (a `char`); out of range or an invalid sequence throws a runtime
  error.
- **Counting**: `s.charCount()` returns the code-point count (contrast
  `s.length()`, bytes).
- **Parsing and construction**: `"65".toChar()` strictly parses a decimal
  code point as a `char`; in `"x" + 'y'` the char joins the string as its
  UTF-8 encoding.

### Null

A string slot may hold the null handle (0). Unlike a null class reference
(which throws on member access), a null string **reads as the empty string**
`""` — calling `s.length()` on a null string returns 0. See
[Class](class.md) for the contrasting class null semantics.

### String methods

The standard library provides string methods (substring, search, case,
splitting, `toInt`/`toFloat`, …). See [string methods](stdlib-string.md).
