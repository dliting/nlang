# 常见错误消息

NLang编译器的错误消息按错误族归类。每族给出触发形态、报错原文、一句话原因与处置入口。系统性限制（「能做什么」而非「怎么报错」）见[已知限制](known-limitations.md)；高频问题的速答见[常见问题](../getting-started/faq.md)。

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

数组值只有两类合法去向——自身数组类型与全部`string`目标（`toString`等），其余标量上下文一律报错拒绝。详见 → [已知限制](known-limitations.md)「标量上下文中的数组值」条。

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

`if`/`while`/`do-while`/`for`/`assert`的条件与`&&`/`||`/`!`的运算数都必须是`bool`（比较与谓词已经产生bool）——int、string、float、char、class、struct、array都会被明确拒绝（`if condition must be bool, not
"Int32"`、`operator '&&' requires bool operands, got "Int32"`）。请改写为显式比较：`if (s != "")`、`if (count != 0)`。详见 →
[语句](statements.md)「条件类型」与[运算符](operators.md)「逻辑」。

### import与可见性

```nlang
import nosuch;
```

```text
Error: Module 'nosuch' not found. Check the project Sources list or -I import path.
```

`import`只开放限定名。未`import`就引用标准库包的成员（以及已知工程模块的成员）会报「未导入」——例如未`import io;`就调用`io.print`，报`Package 'io' is not imported. Add 'import io;' at the top of this
file.`。详见 → [声明](declarations.md)「import声明」。

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

`.ncu`类型描述符只承载数据类型——被导入函数的非常量折叠默认值、以及跨模块函数值引用（`func`签名在描述符文法之外）都在消费侧被拒；函数值形态报`function "pick" does not match the signature of
"func<Int32, Int32>"`。详见 → [默认参数](default-parameters.md)与[已知限制](known-limitations.md)「被导入函数的默认参数」「跨模块函数值被拒绝而非搬运」条。

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

控制流语句的裸体（无花括号）不能是声明——请改用花括号体。调用带`out`形参的函数时，调用点须带`out`标记，否则报`The function
invoke "foo(y)" is not compatible with the declaration.`。详见 →
[语句](statements.md)「无大括号的裸语句体」与[out参数](out-parameters.md)。

### 赋值目标

```nlang
class Counter {
    public int n;
    public int step() { n = n + 1; return n; }
}

int main() {
    Counter c = Counter();
    c.step() += 1;
    return c.n;
}
```

```text
Error: cannot assign to the result of a method call.
```

方法调用结果是值，不是存储位置——`c.step() = v`与`c.step() += v`都报此错。要写字段，左值写`c.n`；支持的全部左值形态见[复合赋值](compound-assignment.md)。

### The method "f" claims the virtual dispatch slot of ...

```nlang
class A {
    public virtual int f(int x) { return x; }
}
class B : A {
    public int f(float x) { return 1; }
}

int main() {
    return 0;
}
```

```text
Error: The method "f" claims the virtual dispatch slot of "A.f(Int32): Int32" with a different signature "f(Float): Int32". Align the signature to override, or rename the method.
	See also the declaration of "A.f(Int32): Int32".
Error: The class "B" does not implement the method "f(Int32): Int32" required by its inheritance contract.
```

子类方法与祖先虚方法或接口契约同名但签名不同。虚分派按名进行，签名不一致会按位错读实参。不符的声明处报主消息（末句即修正提示）并附一行`See also the declaration of ...`指向契约声明，类声明处再跟一条`required by its inheritance contract`；契约是接口方法时锚点写作`"IF.val(Int32): Int32"`（见下一条）。修正签名与契约完全一致（覆写），或给方法改名。详见 → [类](class.md)「虚方法与覆写」。

### The class "F" does not implement the method "val(Int32): Int32" required by interface "IF"

```nlang
interface IF {
    public int val(int x);
}
class F implements IF {
    public int other() { return 0; }
}

int main() {
    return 0;
}
```

```text
Error: The class "F" does not implement the method "val(Int32): Int32" required by interface "IF".
```

```nlang
interface IF {
    public int val(int x);
}
class F implements IF {
    public int val(string s) { return 1; }
}

int main() {
    return 0;
}
```

```text
Error: The method "val" claims the virtual dispatch slot of "IF.val(Int32): Int32" with a different signature "val(String): Int32". Align the signature to override, or rename the method.
	See also the declaration of "IF.val(Int32): Int32".
Error: The class "F" does not implement the method "val(Int32): Int32" required by its inheritance contract.
```

类经`implements`声明实现的接口方法缺失或签名不符。缺失时类声明处报接口锚定的消息；实现存在但签名不符时，报`claims the virtual dispatch slot`族（同上一条，锚点为接口方法）；不符的实现继承自父类（类内未重声明）时改为报`The class "C" does not implement the method "val(Int32): Int32" required by interface "IF" (found "val(String): Int32" with a different signature).`。在类（或其父类）中提供完全一致签名的实现；注意同名方法不得与接口契约重载并存。详见 → [接口](interface.md)「实现契约」。

### switch与enum

```nlang
int main() {
    List<int> xs = {};
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

switch判别式可以是整型家族（含char）、`float`/`double`、`string`或enum；bool、class、struct、`List`、`Dict`以及数组值被拒——class值用`if`/`else` + `equals()`代替，bool判别请展开为`if`/`else`。详见→ [switch](switch.md)。

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

整型**字面量**赋给更窄目标时值必须在目标值域内（常量适配特例，`byte b = 5`合法）；无后缀小数与指数形式是`double`字面量，整数目标不接受浮点常量（`int x = 2e5`同样报Incompatible type）；字符串插值只支持`${name}`具名变量，不支持表达式。详见 →
[基本类型](primitives.md)「数字字面量」与[字符串](string.md)「字符串插值」。

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

`int`/`long`与`ulong`混合的算术没有隐式公共类型——先显式统一符号域（`i as ulong`或`u as long`）；char不与数值隐式转换（比较先`c as int`取码点，算术同样如此）；int/uint/long/ulong→float与long/ulong→double的隐式赋值触发**有损警告**（不影响退出码，`ncc --no-warn`整体抑制，显式`as`不警告）。详见 →
[类型语义](type-semantics.md)「数值转换矩阵」与[运算符](operators.md)「算术」。

### `as`转换

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

`as`只用于转换矩阵不放行的对——对已经隐式合法的加宽写`as`是冗余（`ubyte→int`直接`int i = b;`）；string→数值的`as`被禁止——用`s.toInt()`/`toLong()`/`toDouble()`方法族。详见 →
[类型强制转换](type-casts.md)。

### 字典初始化器键

```nlang
int main() {
    Dict<int, int> d = {"1": 2};
    return 0;
}
```

```text
Error: the Dict collection initializer requires string keys; use set() with an explicit 'Int32' key
```

集合初始化器把字典键按字符串常量发射并按K的标签装箱——这只对`K = string`正确。非string键的`Dict<K,V>`用空初始化器构造后`set()`逐条填入。详见 →
[集合初始化器](collection-initializers.md)。

### struct没有构造函数

```nlang
struct Point { int x; int y; }
int main() {
    Point p = Point(2, 3);
    return 0;
}
```

```text
Error: The function "Point" does not exist or is not accessible.
```

struct没有构造函数——`Point(2, 3)`被当作按名调用函数，解析落空。struct的创建形式是花括号条目：具名`Point{x: 2, y: 3}`或位置`Point{2, 3}`（见[结构体](struct.md)与[集合初始化器](collection-initializers.md)）。

### 泛型花括号锚定

```nlang
int main() {
    List<int> nums = List<int>{60};
    return 0;
}
```

```text
Error: syntax error
Error: Invalid statement.
```

泛型类型的锚定头（`List<int>{...}`）无法解析。给字面量一个带类型声明即可：`List<int> nums = [60];`。详见 → [已知限制](known-limitations.md)「泛型实例化没有锚定头」条与[集合初始化器](collection-initializers.md)。

### 类型别名

```nlang
using A = int;
using A = float;
```

```text
Error: The type alias "A" is defined more than once in this translation unit.
```

同一翻译单元内别名名不可重复；循环别名（`using A = B; using B = A;`）被拒。详见 → [声明](declarations.md)「类型别名」。

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

`.n`源文件与`.nproj`项目文件在词法前做严格Unicode转换格式（UTF-8，Unicode Transformation Format）校验（执行点是ncc；nide的构建经由ncc，同样执行该校验）：无效字节会得到明确报错（含第一个无效字节所在行号），UTF-16保存的文件得到专门提示（`Source file ... is UTF-16, not UTF-8.`）——旧编码字节不会混入字符串常量。文件开头的UTF-8字节顺序标记（BOM，byte order mark）被接受并跳过。详见 →
[基本类型](primitives.md)「表示链：从源码到控制台」。
