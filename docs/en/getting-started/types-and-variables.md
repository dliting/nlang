# Crash Course: Types and Variables

Every snippet below really compiles and runs (`ncc build` + `nvm`) —
copy any of them into nide or a `.n` file to try it. Each section states
the actual output and exit code, and ends with links to the matching
Language Specification chapters.

### Variables and Types

```nlang
import io;

int main() {
    int level = 3;              // every variable declares its type; no inference
    float scale = 1.5;
    string title = "demo";
    const int MAX = 100;        // const: assigning after initialization is a compile error
    io.print(title + ": " + level * 10 + " / " + scale);
    if (level * 10 == 30)
        return 30;              // exit code 30
    return 1;
}
```

Output `demo: 30 / 1.5`. The primitive types are `int` (32-bit integer),
`float` (32-bit float), and `string` (UTF-8 bytes, reference semantics);
for the compound types enum, struct, and class see the declarations
chapter.

See also: [Language Specification / Types](../language-spec/types.md),
[Type Semantics](../language-spec/type-semantics.md),
[Declarations](../language-spec/declarations.md).

### Strings

```nlang
import io;

int main() {
    string who = "NLang";
    int year = 2026;
    io.print("hello, ${who}");      // interpolation
    io.print("year: " + year);      // int converts to string on concatenation
    if ("${who} ${year}" == "NLang 2026")
        return 9;
    return 1;
}
```

Output `hello, NLang` and `year: 2026`; exit code 9. Strings are UTF-8
byte sequences, and `length()`/`substring()`/`indexOf()` all count
bytes.

See also: [Language Specification / Types](../language-spec/types.md),
[Standard Library](../language-spec/standard-library.md).
