# Types


NLang is statically typed: every variable, field, parameter, and return
value has a declared type, and the compiler checks each expression against
it. This section maps every type in the language to its own page. Each
page is self-contained — how to declare the type, its runtime semantics,
and how it behaves in expressions and statements.

### Type inventory

| Type        | Category                    | Page |
|-------------|-----------------------------|------|
| `int`       | primitive — value type        | [Primitives](primitives.md) |
| `float`     | primitive — value type        | [Primitives](primitives.md) |
| `string`    | primitive — immutable object  | [String](string.md) |
| `enum`      | composite — value (int32)     | [Enum](enum.md) |
| `struct`    | composite — value (deep copy) | [Struct](struct.md) |
| `class`     | composite — reference         | [Class](class.md) |
| `interface` | composite — reference         | [Interface](interface.md) |
| `T[]`       | composite — reference (heap)  | [Array](array.md) |
| `List<T>`   | built-in generic — reference  | [Built-in Generic Classes](builtin-generic-classes.md) |
| `Dict<K,V>` | built-in generic — reference  | [Built-in Generic Classes](builtin-generic-classes.md) |
| `Object`    | reference — boxed             | [Object & Boxing](object.md) |
| `Func<...>` | first-class function value    | [Functions](functions.md) |

### Where to look

- **What a type is, and how to declare it** — the per-type pages above.
- **How each type behaves on assignment, parameter passing, return, and as
  a field or array element** — [Type Semantics](type-semantics.md), a
  complete per-type summary.
- **How values convert between types** (explicit casts, `as`, coercion to
  string) — [Type Casts](type-casts.md).
- **Declaring non-type things** (imports, variables, type aliases) —
  [Declarations](declarations.md).
