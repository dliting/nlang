# 常见错误消息

NLang 编译器的具名诊断按错误族归类。每族给出触发形态、报错原文、
一句话原因与处置入口。系统性限制（「能做什么」而非「怎么报错」）见
[已知限制](known-limitations.md)；高频问题的速答见
[常见问题](../getting-started/faq.md)。每族完整拒绝形态见 e2e 对应
`compile_error` 用例。

### 数组值（标量上下文）

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

数组值只有两类合法去向——自身数组类型与全部 `string` 目标（`toString`
等），其余标量上下文一律具名拒绝。详见 → [已知限制](known-limitations.md)
「标量上下文中的数组值」条。完整形态见 e2e 用例
`assign_array_to_int_reject` 等（`*_array_*_reject` 族）。

### 条件与逻辑运算

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

`if`/`while`/`do-while`/`for`/`assert` 的条件与 `&&`/`||`/`!` 的运算数
都必须是 `int`（比较产生 `int`）——string、float、class、struct、
array 都是具名拒绝（`operator '&&' requires int operands, got
"String"`）。详见 → [语句](statements.md)「条件类型」与 [表达式](expressions.md)「逻辑」。
完整形态见 e2e 用例 `cond_*`、`condition_array_reject`、
`logical_operand_*`、`not_operand_*`。

### import 与可见性

```nlang
import nosuch;
```

```text
Error: Module 'nosuch' not found. Check the project Sources list or -I import path.
```

`import` 只开放限定名。未 `import` 就引用内建命名空间的成员（以及已知
工程模块的成员）会报「未导入」——例如未 `import io;` 就调用 `io.print`，
报 `Package 'io' is not imported. Add 'import io;' at the top of this
file.`。详见 → [声明](declarations.md)「import 声明」。完整形态见 e2e
用例 `import_not_found`、`import_io_missing`、`import_dotted_singlefile`、
`import_string_form`、`unresolved_import_call`（import 族无统一前缀）。

### 默认参数与函数值

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

`.ncu` 类型描述符只承载数据类型——被导入函数的非常量折叠默认值、
以及跨模块函数值引用（`Func` 签名在描述符文法之外）都在消费侧被拒
（`function "pick" does not match the signature of "Func<Int32, Int32>"`）。
详见 → [函数](functions.md)「默认参数」与 [已知限制](known-limitations.md)
「被导入函数的默认参数」「跨模块函数值被拒绝而非搬运」条。完整形态见
e2e 用例 `cross_mod_default_*`、`func_*`。

### 声明与控制流

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

控制流语句的裸体（无花括号）不能是声明——请改用花括号体。调用带
`out` 形参的函数时，调用点须带 `out` 标记，否则报 `The function
invoke "foo(y)" is not compatible with the declaration.`。详见 →
[语句](statements.md)「控制流」与 [函数](functions.md)「out 参数」。完整
形态见 e2e 用例 `local_decl_unbraced_reject`、`out_*`。

### switch 与 enum

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

switch 判别式只能是 `int`/`float`/`string`/enum；class、struct、
`List`、`Dict` 以及数组值被拒——用 `if`/`else` + `equals()` 代替。详见
→ [语句](statements.md)「switch」。完整形态见 e2e 用例 `switch_*`、
`array_elem_switch*`、`array_elem_dict_value_switch_reject`、`enum_*`。

### 字面量

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

整型科学计数法字面量超出 `int32` 时词法器拒绝（`1e30` → 语法错误，
不静默截断指数）；字符串插值只支持 `${name}` 具名变量，不支持表达式。
详见 → [表达式](expressions.md)「字符串插值」与转义序列。完整形态见
e2e 用例 `int_sci_*`、`string_escape_*`、`interp_*`。

### 类型别名

```nlang
using A = int;
using A = float;
```

```text
Error: The type alias "A" is defined more than once in this translation unit.
```

同一翻译单元内别名名不可重复；循环别名（`using A = B; using B = A;`）
被拒。详见 → [声明](declarations.md)「类型别名」。完整形态见 e2e 用例
`alias_*`。
