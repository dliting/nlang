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
Error: if condition must be bool, not "String".
```

`if`/`while`/`do-while`/`for`/`assert` 的条件与 `&&`/`||`/`!` 的运算数
都必须是 `bool`（比较与谓词已经产生 bool）——int、string、float、char、
class、struct、array 都是具名拒绝（`if condition must be bool, not
"Int32"`、`operator '&&' requires bool operands, got "Int32"`）。请改写
为显式比较：`if (s != "")`、`if (count != 0)`。详见 →
[语句](statements.md)「条件类型」与 [表达式](expressions.md)「逻辑」。
完整形态见 e2e 用例 `cond_*`、`condition_array_reject`、
`logical_operand_*`、`not_operand_*`。

### import 与可见性

```nlang
import nosuch;
```

```text
Error: Module 'nosuch' not found. Check the project Sources list or -I import path.
```

`import` 只开放限定名。未 `import` 就引用标准库包的成员（以及已知
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

switch 判别式可以是整型家族（含 char）、`float`/`double`、`string` 或
enum；bool、class、struct、`List`、`Dict` 以及数组值被拒——class
值用 `if`/`else` + `equals()` 代替，bool 判别请展开为 `if`/`else`。
详见
→ [语句](statements.md)「switch」。完整形态见 e2e 用例 `switch_*`、
`array_elem_switch*`、`array_elem_dict_value_switch_reject`、`enum_*`。

### 字面量与常量适配

```nlang
int main() {
    byte b = 1000;
    return 0;
}
```

```text
Error: constant 1000 out of range for 'byte'
```

```nlang
int main() {
    int x = 1e30;
    return 0;
}
```

```text
Error: Incompatible type "1e+30".
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

整型**字面量**赋给更窄目标时值必须在目标值域内（常量适配特例，
`byte b = 5` 合法）；无后缀小数与指数形式是 `double` 字面量，整数
目标不接受浮点常量（`int x = 2e5` 同样报 Incompatible type）；字符串
插值只支持 `${name}` 具名变量，不支持表达式。详见 →
[基本类型](primitives.md)「数字字面量」与 [表达式](expressions.md)
「字符串插值」。完整形态见
e2e 用例 `int_sci_*`、`string_escape_*`、`interp_*`、
`int_family_literal_tier`、`int_family_constant_fit*`。

### 数值类型混合

```nlang
int main() {
    int i = 1;
    ulong u = 2;
    long x = i + u;
    return 0;
}
```

```text
Error: no implicit common type for "Int32" and "ULong".
```

```nlang
int main() {
    char c = 'a';
    bool z = c < 100;
    return 0;
}
```

```text
Error: a char value can only be compared with a char value.
```

```nlang
int main() {
    long l = 5000000000;
    float f = l;
    return 0;
}
```

```text
Warning: implicit conversion from 'Long' to 'Float' loses precision
```

`int`/`long` 与 `ulong` 混合的算术没有隐式公共类型——先显式统一符号域
（`i as ulong` 或 `u as long`）；char 不与数值隐式转换（比较先
`c as int` 取码点，算术同样如此）；int/uint/long/ulong→float 与
long/ulong→double 的隐式赋值触发**有损警告**（不影响退出码，
`ncc --no-warn` 整体抑制，显式 `as` 不警告）。详见 →
[类型语义](type-semantics.md)「数值转换矩阵」与
[运算符](operators.md)「算术」。完整形态见 e2e 用例
`int_family_promote*`、`int_family_lossy_warn`、`ncc_no_warn`、
`char_numeric_cmp_reject`。

### `as` 转换

```nlang
int main() {
    ubyte b = 200;
    int i = b as int;
    return 0;
}
```

```text
Error: `as` cannot perform implicit conversion `UByte` → `Int32`.
```

```nlang
int main() {
    string s = "5";
    int x = s as int;
    return 0;
}
```

```text
Error: Invalid cast: `String as Int32` is not allowed.
```

`as` 只用于转换矩阵不放行的对——对已经隐式合法的加宽写 `as` 是冗余
（`ubyte→int` 直接 `int i = b;`）；string→数值的 `as` 被禁止——用
`s.toInt()`/`toLong()`/`toDouble()` 方法族。详见 →
[类型强制转换](type-casts.md)。完整形态见 e2e 用例 `cast_*`
（含 `char_cast`、`char_cast_invalid`）。

### 字典初始化器键

```nlang
int main() {
    Dict<int, int> d = new Dict<int, int>{"1": 2};
    return 0;
}
```

```text
Error: the Dict collection initializer requires string keys; use set() with an explicit 'Int32' key
```

集合初始化器把字典键按字符串常量发射并按 K 的标签装箱——这只对
`K = string` 正确。非 string 键的 `Dict<K,V>` 用空初始化器构造后
`set()` 逐条填入。详见 →
[集合初始化器](collection-initializers.md)。完整形态见 e2e 用例
`dict_init_nonstring_key_reject`。

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

### 源文件编码

```nlang
int main() {
    string s = "中文内容";   // 文件被编辑器以 GBK/ANSI 编码保存
    return 0;
}
```

```text
Error: Source file src_not_utf8.n is not valid UTF-8 (first invalid byte at line 2). Save the file as UTF-8.
```

`.n` 源文件与 `.nproj` 项目文件在词法前做严格 UTF-8 校验（执行
点是 ncc；nide 的构建经由 ncc，同样受此门控）：无效
字节被具名拒绝（报错带第一个无效字节所在行号），UTF-16 保存的
文件得到专门提示（`Source file ... is UTF-16, not UTF-8.`）——旧
编码字节不会再静默混入字符串常量。文件开头的 UTF-8 BOM 被接受
并跳过。详见 →
[基本类型](primitives.md)「表示链：从源码到控制台」。完整形态见
e2e 用例 `src_not_utf8`（源文件）与 ctest 守卫 `nproj_utf8`
（项目文件）。
