> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 6.7 控制流

规范性规则见§6.3与§6.5。

### If

```nlang
if (cond) { body }
if (cond) { body } else { body }
```

条件为`true`时执行第一个语句体；否则执行`else`语句体（若存在）。条件必须是`bool`（见[语句](index.md)「条件类型」）。`else`之后可跟随另一个`if`以形成链式判断——`else`语句体就是一个`if`语句；没有独立的`else if`关键字（单词`elseif`形式是语法错误）。

语句体是复合语句：在其中声明的变量只在该语句体内可见（块作用域）。

```nlang
int n = 7;
if (n > 10) {
    n = 1;
} else if (n > 5) {
    n = 2;
} else {
    n = 3;
}
// n == 2
```

### Switch

```nlang
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

判断量必须是整型、浮点型、`char`、`string`、或enum类型值（enum按其int值比较）。class、struct、数组、`bool`、`null`判断量在编译期被拒绝——每种类型能是什么见[类型](../04-types/index.md)。

**类型化相等。**比较使用判断量所属族的相等运算符：int与enum精确比较；float按IEEE语义比较（`-0.0 == 0.0`为真，NaN永远不等于任何东西，包括它自己）；string按内容比较，不按同一性。case标签必须与判断量同族——不做跨族转换（对int判断量的`case "1"`是编译错误）——且常量标签必须落在判断量自身类型的取值范围内（对`byte`判断量的`case 300`是编译错误）。`null`不是合法的case标签。标签可以是计算表达式（例如`case f(x):`）——在运行期按子句顺序求值。

**多值标签。**一个case子句可列出多个标签（`case 1, 2:`）；任一匹配时执行语句体。

**不穿透。**每个case语句体以隐式跳出switch的跳转结束——即使没有显式`break`语句，执行也不会级联到下一个case语句体。这与Java/C#语义一致，与C/C++不同。`break`关键字仅在从多语句case语句体内部提前退出时才需要。case语句体内的`break`永远绑定到switch本身，绝不绑定到包围的循环。

**重复标签。**同一个switch内（跨子句或在一个多值子句内）值相同的常量标签（数字/字符串字面量和enum成员）在编译期被拒绝（`case 1:`加上`case Color.Red:`（`Red = 0`）就是重复）。非常量标签重复（两个调用都返回1）是允许的；第一个匹配生效。

### Assert

```nlang
assert(condition);
```

求值`condition`。若为假，抛出一个可被`try/catch`块捕获的`AssertionException`。若未捕获，以退出码1终止程序。仅单实参形式（不支持自定义失败消息）。条件必须是`bool`（见[语句](index.md)「条件类型」）。

```nlang
int x = 5;
assert(x > 3);            // 通过 —— 无事发生
try {
    assert(x > 10);        // 抛出AssertionException
} catch (AssertionException e) {
    // 被捕获
}
```

见[异常](../08-exceptions/index.md)了解`AssertionException`类与`try/catch`机制。
