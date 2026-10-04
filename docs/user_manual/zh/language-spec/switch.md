# Switch


```nlang
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

判断量必须是 `int`、`float`、`string`、或 enum 类型值（enum 按其 int 值比
较）。class、struct、数组、`null` 判断量在编译期被拒绝——共享的 `int` 规则
见 [语句](statements.md)「条件类型」，每种类型能是什么见
[类型语义](type-semantics.md)。

**类型化相等。** 比较使用判断量所属族的相等运算符：int 与 enum 精确比较；
float 按 IEEE 语义比较（`-0.0 == 0.0` 为真，NaN 永远不等于任何东西，包括它
自己）；string 按内容比较，不按同一性。case 标签必须与判断量同族——不做跨族
转换（对 int 判断量的 `case "1"` 是编译错误）。`null` 不是合法的 case 标签。
标签可以是计算表达式（例如 `case f(x):`）——在运行期按子句顺序求值。

**多值标签。** 一个 case 子句可列出多个标签（`case 1, 2:`）；任一匹配时执行
语句体。

**不穿透。** 每个 case 语句体以隐式跳出 switch 的跳转结束——即使没有显式
`break` 语句，执行也不会级联到下一个 case 语句体。这与 Java/C# 语义一致，与
C/C++ 不同。`break` 关键字仅在从多语句 case 语句体内部提前退出时才需要。
case 语句体内的 `break` 永远绑定到 switch 本身，绝不绑定到包围的循环。

**重复标签。** 同一个 switch 内（跨子句或在一个多值子句内）值相同的常量标签
（数字/字符串字面量和 enum 成员）在编译期被拒绝（`case 1:` 加上 `case
Color.Red:`（`Red = 0`）就是重复）。非常量标签重复（两个调用都返回 1）是允
许的；第一个匹配生效。
