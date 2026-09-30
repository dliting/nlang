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
    double scale = 1.5;         // unsuffixed decimal is double; float is 1.5f
    long total = 5000000000;    // 64-bit integer
    char grade = 'A';
    bool ok = level * 10 == 30; // comparisons produce bool
    string title = "demo";
    const int MAX = 100;        // const: assigning after initialization is a compile error
    io.print(title + ": " + level * 10 + " / " + scale);
    io.print(grade + " " + ok + " " + total);
    if (ok)
        return 30;              // exit code 30
    return 1;
}
```

Output `demo: 30 / 1.5` and `A true 5000000000`. There are 12 scalar
primitives: the integer family (`byte` `ubyte` `short` `ushort` `int`
`uint` `long` `ulong`), `float`/`double`, `bool`, and `char` (a Unicode
code point); `string` is the 13th primitive (UTF-8 bytes, reference
semantics). The compound types enum, struct, and class are covered in the
sections below and in the declarations chapter.

See also: [Language Specification / Types](../language-spec/types.md),
[Type Semantics](../language-spec/type-semantics.md),
[Declarations](../language-spec/declarations.md).

### Enum

```nlang
import io;

enum Color { Red, Green, Blue }

int main() {
    Color c = Color.Blue;
    int n = c;                   // enum values are integers
    io.print(c.toString());       // "Blue"
    io.print(n);                  // 2
    switch (c) {
        case Color.Red: return 1;
        case Color.Green: return 2;
        case Color.Blue: return 3;
    }
}
```

Output `Blue` and `2`; exit code 3. Enum values are `int` constants the
compiler assigns (auto-increment from 0 unless you give them explicit
values); a value converts to `int` directly, `toString()` returns the
member name, and an enum is a valid `switch` discriminant.

See also: [Language Specification / Declarations](../language-spec/declarations.md).

### Struct

```nlang
import io;

struct Point {
    int x;
    int y;
}

int main() {
    Point p;                     // zero-initialized
    p.x = 3;
    p.y = 4;
    Point q = p;                 // deep copy (value semantics)
    q.x = 9;                     // does not affect p
    io.print(p.x + p.y);          // 7
    if (q.x == 9 && p.x == 3)
        return 34;
    return 1;
}
```

Output `7`; exit code 34. A struct is a value type: declaring one
zero-initializes its fields, and copying it (`Point q = p;`) copies the
whole struct — including any nested structs — so the copies are
independent. Unlike a class, a struct has no methods.

See also: [Language Specification / Declarations](../language-spec/declarations.md).

### Type alias

```nlang
import io;

using Ints = int[];

int main() {
    Ints xs = [10, 20, 30];
    io.print(xs[0] + xs.length);  // 10 + 3 = 13
    if (xs.length == 3)
        return 13;
    return 1;
}
```

Output `13`; exit code 13. `using Name = Type;` gives a type a second
name; the alias expands textually at each use site, so `Ints xs = ...` is
exactly `int[] xs = ...`. Aliases work over primitive, array, generic, and
`Func<...>` type forms.

See also: [Language Specification / Declarations](../language-spec/declarations.md).

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
bytes; code-point access (`charAt`, `charCount`, `foreach (char c in s)`)
is on the String page under "The char bridge".

See also: [Language Specification / Types](../language-spec/types.md),
[Standard Library](../language-spec/standard-library.md).
