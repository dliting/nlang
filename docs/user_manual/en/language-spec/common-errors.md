# Common Error Messages

The NLang compiler's named diagnostics, grouped by error family. Each
family shows the trigger shape, the error text, a one-line cause, and a
pointer to the full rule. Systematic limits (what you *can* do, rather
than how it errors) are on [Known Limitations](known-limitations.md);
quick answers to high-frequency questions are on the [FAQ](../getting-started/faq.md).
Each family's full set of rejections is covered by the corresponding
e2e `compile_error` cases.

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
a named rejection. See → [Known Limitations](known-limitations.md), the
"Array values in scalar contexts" item. Full forms: e2e cases
`assign_array_to_int_reject` and the `*_array_*_reject` family.

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
named rejections (`if condition must be bool, not "Int32"`,
`operator '&&' requires bool operands, got "Int32"`). Rewrite as an
explicit comparison: `if (s != "")`, `if (count != 0)`. See →
[Statements](statements.md) "Condition typing" and
[Expressions](expressions.md) "Logical". Full forms: e2e cases `cond_*`,
`condition_array_reject`, `logical_operand_*`, `not_operand_*`.

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
[Declarations](declarations.md) "Import Declaration". Full forms: e2e
cases `import_not_found`, `import_io_missing`, `import_dotted_singlefile`,
`import_string_form`, `unresolved_import_call` (no common prefix).

### Default parameters and function values

```nlang
// lib.n
int helper() { return 1; }
int g(int a = helper()) { return a; }

// main.n
import lib;
int main() {
    return lib.g();
}
```

```text
Error: imported function 'g' has a non-constant-foldable default for parameter 0; cross-module defaults must be literal (int/float/string/null/negative)
```

The `.ncu` type descriptor carries only data types — an imported
function's non-constant-foldable default, and a cross-module function
value reference (a `Func` signature is beyond the descriptor syntax),
are both rejected on the consumer side
(`function "pick" does not match the signature of "Func<Int32, Int32>"`).
See → [Functions](functions.md) "Default Parameters" and [Known
Limitations](known-limitations.md), the "Default parameters on imported
functions" and "Cross-module function values are rejected, not
transported" items. Full forms: e2e cases `cross_mod_default_*`,
`func_*`.

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
→ [Statements](statements.md) "Control Flow" and [Functions](functions.md)
"Out Parameters". Full forms: e2e cases `local_decl_unbraced_reject`,
`out_*`.

### switch and enum

```nlang
int main() {
    List<int> xs = new List<int>();
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
→ [Statements](statements.md) "Switch". Full forms: e2e cases
`switch_*`, `array_elem_switch*`, `array_elem_dict_value_switch_reject`,
`enum_*`.

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
literals" and [Expressions](expressions.md) "String interpolation".
Full forms: e2e cases `int_sci_*`, `string_escape_*`, `interp_*`,
`int_family_literal_tier`, `int_family_constant_fit*`.

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
[Operators](operators.md) "Arithmetic". Full forms: e2e cases
`int_family_promote*`, `int_family_lossy_warn`, `ncc_no_warn`,
`char_numeric_cmp_reject`.

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
`toDouble()` method family. See → [Type Casts](type-casts.md). Full
forms: e2e cases `cast_*` (including `char_cast`, `char_cast_invalid`).

### Dict initializer keys

```nlang
int main() {
    Dict<int, int> d = new Dict<int, int>{"1": 2};
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
[Collection Initializers](collection-initializers.md). Full forms: e2e
case `dict_init_nonstring_key_reject`.

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
[Declarations](declarations.md) "Type Aliases". Full forms: e2e cases
`alias_*`.

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
ncc, so building there is gated the same way): invalid bytes are
rejected with a named error (the
line of the first invalid byte included) and a UTF-16 save gets a
dedicated hint (`Source file ... is UTF-16, not UTF-8.`) — legacy
encoding bytes no longer slip silently into string constants. A
leading UTF-8 byte order mark (BOM) is accepted and skipped. See →
[Primitives](primitives.md) "Representation: from source to console".
Full forms: e2e case `src_not_utf8` (sources) and the ctest guard
`nproj_utf8` (project files).
