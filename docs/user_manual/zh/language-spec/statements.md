# 语句


NLang的控制流语句。本页覆盖它们共享的规则；每个具体语句都有独立页面。

### 语句页面

| 语句                                              | 页面 |
|---------------------------------------------------|------|
| `if` / `else`                                     | [If](if.md) |
| `for` / `while` / `do-while`                      | [循环](loops.md) |
| `foreach`                                         | [Foreach](foreach.md) |
| `switch` / `case` / `default`                     | [Switch](switch.md) |
| `try` / `catch` / `throw` / `finally`             | [异常](exception.md) |
| `assert`                                          | [Assert](assert.md) |
| 复合赋值（`+=` `-=` `*=` `/=` `%=`）                | [复合赋值](compound-assignment.md) |
| `const`局部变量                                   | [Const局部](const-local.md) |

### 条件类型

`if`/`while`/`do-while`/`for`/`assert`的条件必须是**`bool`**——比较与谓词已经产生bool（见[运算符](operators.md)、[基本类型](primitives.md)）。int、string、float、char、class、struct、数组条件都是编译错误（`if condition must be bool, not "Int32"`）。NLang是严格bool语言，没有C式的「非零即真」：`if (1)`与`if (count)`都不合法，请写`if (count != 0)`；`if (s != "")`、`if (obj != null)`同理。

### `break`、`continue`、`return`

- `break;` —— 退出包围的循环（或`switch`），跳过任何步进子句。
- `continue;` —— 跳到下一轮循环（`for`中先执行步进子句）。
- `return;` / `return expr;` —— 退出包围的函数。

每个语句页面在其定义该结构处重复这些规则。

### 无大括号的裸语句体

控制流语句的无大括号（裸）语句体不能是变量声明——`if (c) int x = 1;`会报`a local declaration cannot be the unbraced body of a control-flow
statement`。请改用带大括号的语句体。
