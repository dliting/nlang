# 速览: 控制流
### 控制流

```nlang
import io;

int classify(int n) {
    switch (n) {
        case 0:
            return 100;
        case 1, 2:              // 多值标签；case 体不穿透
            return 200;
        default:
            return 300;
    }
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

输出 `200`，退出码 25。`switch` 支持多值标签与 `int`/`float`/`string`/enum
四种判别族，case 体默认不穿透；`foreach` 可遍历数组、`List<T>` 与 `Dict` 的键。

详见 → [语言规格/语句](../language-spec/statements.md)。

### 异常

```nlang
import io;

int risky(int mode) {
    try {
        if (mode == 1)
            throw new Exception("manual");
        int[] a = new int[2];
        return a[9];                // 越界 → IndexOutOfBoundsException
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

输出 `finally always runs`、`boom`，退出码 5。内建异常类有
`NullPointerException`、`DivByZeroException`、`IndexOutOfBoundsException`、
`AssertionException`，都继承自 `Exception`；用户类也可 `extends Exception`。

详见 → [语言规格/语句](../language-spec/statements.md)。

