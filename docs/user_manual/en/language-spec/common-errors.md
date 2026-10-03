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
Error: if condition must be int, got "String".
```

The condition of `if`/`while`/`do-while`/`for`/`assert` and the operand
of `&&`/`||`/`!` must all be `int` (comparisons produce `int`) — string,
float, class, struct, and array are named rejections
(`operator '&&' requires int operands, got "String"`). See →
[Statements](statements.md) "Condition typing" and [Expressions]
(expressions.md) "Logical". Full forms: e2e cases `cond_*`,
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

A switch discriminant can only be `int`/`float`/`string`/enum; class,
struct, `List`, `Dict`, and array values are rejected — use `if`/`else` +
`equals()` instead. See → [Statements](statements.md) "Switch". Full
forms: e2e cases `switch_*`, `array_elem_switch*`,
`array_elem_dict_value_switch_reject`, `enum_*`.

### Literals

```nlang
int main() {
    int x = 1e30;
    return 0;
}
```

```text
Error: syntax error
Error: Invalid statement.
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

An integer scientific-notation literal that exceeds `int32` is rejected
by the lexer (`1e30` → syntax error; the exponent is not silently
truncated); string interpolation only supports `${name}` identifiers,
not expressions. See → [Expressions](expressions.md) "String
interpolation" and the escape-sequence table. Full forms: e2e cases
`int_sci_*`, `string_escape_*`, `interp_*`.

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
