# 循环


NLang有三种循环形式：`for`、`while`、`do-while`。它们共享`bool`条件规则与`break`/`continue`语义（见[语句](statements.md)「条件类型」）。

### `for`

```nlang
for (init; cond; fini) { body }
```

三段语义：

- **初始化段执行一次**（在第一次条件求值之前）；
- **条件段在每轮循环前求值**（包括第一轮）；仅当它为`true`时执行语句体；
- **步进段在每次语句体执行后运行**（包括循环自然结束前的最后一轮）。

`break`立即退出循环（**跳过步进段**）；`continue`跳到下一轮（**先执行步进段**，然后重新求值条件）。在初始化段声明的变量是**函数作用域**——与`foreach`循环变量一样，两者都不受块作用域限制（见[Foreach](foreach.md)）；语句体内其他声明仅在该体内可见（块作用域）。

```nlang
int sum = 0;
for (int i = 1; i <= 10; i = i + 1) {
    sum = sum + i;
}
// sum == 55；循环后 `i` 仍在作用域内（函数作用域）
```

```nlang
int count = 0;
for (int j = 0; j < 20; j = j + 1) {
    if (j == 5) { break; }        // 退出循环（步进段被跳过）
    if (j % 2 == 1) { continue; } // 步进段运行，然后进入下一轮
    count = count + 1;
}
// count == 3（计入 j = 0, 2, 4 的轮次；j = 5 命中 break）
```

### `while`

```nlang
while (cond) { body }
```

`while` **先判断后执行**——如果条件从一开始就是`false`，语句体可能从不执行。`break`退出循环；`continue`跳到下一轮（回到条件判断）；在语句体内声明的变量仅在该体内可见（块作用域）。

```nlang
int steps = 0;
while (steps < 3) {
    steps = steps + 1;
}
// steps == 3
```

### `do-while`

```nlang
do { body } while (cond);
```

`do-while` **先执行语句体后判断**——即使条件恒为`false`，语句体也至少执行一次。`break`和`continue`行为同`while`。

```nlang
int n = 0;
do {
    n = n + 1;
} while (false);
// n == 1 —— 语句体执行了一次（do-while 至少执行一次）
```
