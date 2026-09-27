# String


`string` is a primitive, but unlike `int`/`float` it is an **immutable
object** referenced by a handle. This page covers the string type end to
end: what it is, its memory semantics, how to build and compare them, and
escape sequences and interpolation inside literals.

### Value semantics

String is a **value type** in the type system: assignment, parameter
passing, and return copy the handle, and `==` compares **content**, not
identity. The object behind the handle is immutable, so sharing a handle is
harmless — two names to the same string can never diverge. (Contrast with
`class`, a reference type: see [Class](class.md) and
[Type Semantics](type-semantics.md).)

### Encoding and length

String literals are stored as their UTF-8 byte sequence in the module string
constant table. `string.length()` returns the **byte count**, not the Unicode
code-point count — `"héllo".length()` is 6 (5 code points, but `é` is 2 bytes
in UTF-8). Proper UTF-8 code-point iteration is deferred to a future release.

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

- **Concatenation**: `string + string` and `string + primitive` (the
  primitive is coerced — see [Type Casts](type-casts.md) "Primitive → String
  Coercion"). `string - string` and the other arithmetic forms are compile
  errors.
- **Interpolation**: `${identifier}` inside a literal — see below.
- **Escape sequences**: see below.
- **Explicit construction from a primitive**: `ToString()` / the primitive →
  string coercion — see [Type Casts](type-casts.md).

### Comparison

`==` / `!=` compare **content**. Relational ordering (`<`, `>`, `<=`, `>=`)
uses C `strcmp`-style byte-by-byte comparison (e.g. `"Z" < "a"` is true because
`'Z'` (90) < `'a'` (97)). Because strings are UTF-8 and UTF-8 byte order
equals code-point order, ordering is also correct for non-ASCII text:
`"é" > "z"` is true. Mixed string/non-string comparison is a compile error
except against the null literal — `s == null` reads the null side as the empty
string, so `"" == null` is true and any non-empty string compares unequal. The
full comparison rules are on [Operators](operators.md) "Comparison".

### Escape sequences

Inside double-quoted literals:

| Escape          | Produces                                              |
|-----------------|------------------------------------------------------|
| `\n` `\r` `\t`  | newline, CR, tab                                      |
| `\\` `\"` `\'`  | backslash, double quote, apostrophe                   |
| `\0` `\a` `\b` `\f` `\v` | NUL, bell, backspace, form feed, vertical tab |
| `\x`/`\u`…      | not supported                                         |

Any other escape (e.g. `\q`) is a compile error — escapes never pass through
as literal backslash pairs. Escapes compose with interpolation:
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
| `int`            | `OP_Int32_to_str`      |
| `float`           | `OP_Float_to_str`      |
| `string`          | none                   |
| `enum`            | `OP_Enum_to_str`       |
| `Array`           | `OP_Array_to_str`      |
| `List` / `Dict`   | `OP_CallMethod "toString"` |
| `class`           | `OP_CallMethod "toString"` |

### Null

A string slot may hold the null handle (0). Unlike a null class reference
(which throws on member access), a null string **reads as the empty string**
`""` — calling `s.length()` on a null string returns 0. See
[Class](class.md) for the contrasting class null semantics.

### String methods

The standard library provides string methods (substring, search, case,
splitting, `toInt`/`toFloat`, …). See [Standard Library](standard-library.md).
