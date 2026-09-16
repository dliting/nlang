# Crash Course: Control Flow
### Control Flow

```nlang
import io;

int classify(int n) {
    int result = 0;
    switch (n) {
        case 0:
            result = 100;       // only this arm runs on a hit
        case 1, 2:              // multi-value label
            result = 200;       // arms exit automatically; no fall-through into default
        default:
            result = 300;
    }
    return result;              // classify(2) = 200; with fall-through this would be 300
}

int main() {
    int total = 0;
    for (int i = 0; i < 3; i = i + 1)
        total = total + i;      // 0+1+2
    while (total < 10)
        total = total + 5;      // 10
    int[] evens = [2, 4, 6];
    foreach (int e in evens)
        total = total + e;      // 25
    io.print(classify(2));      // 200
    if (total == 25)
        return 25;
    return 1;
}
```

Output `200`, exit code 25. `switch` supports multi-value labels and
four discriminator families (`int`/`float`/`string`/enum); case bodies
do not fall through, so no `break` is needed to close one off
(Java/C# semantics); `foreach` iterates arrays, `List<T>`, and the keys
of a `Dict`.

See also: [Language Specification / Statements](../language-spec/statements.md).

### Exceptions

```nlang
import io;

int risky(int mode) {
    try {
        if (mode == 1)
            throw new Exception("manual");
        int[] a = new int[2];
        return a[9];                // out of bounds → IndexOutOfBoundsException
    } catch (IndexOutOfBoundsException e) {
        return 2;
    } finally {
        io.print("finally always runs");
    }
}

int main() {
    if (risky(0) != 2)
        return 1;
    try {
        throw new Exception("boom");
    } catch (Exception e) {
        io.print(e.message);
        return 5;
    }
    return 1;
}
```

Output `finally always runs` and `boom`; exit code 5. The built-in
exception classes `NullPointerException`, `DivByZeroException`,
`IndexOutOfBoundsException`, and `AssertionException` all extend
`Exception`; user classes can `extends Exception` too.

See also: [Language Specification / Statements](../language-spec/statements.md).
