# 表达式


### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除零/模零抛运行期错误——float 除零也不例外，抛错
而不是产生 IEEE 754 的 ±inf/NaN（NLang 在这点上不同于 C/C++/Java）。

**整数溢出**按二进制补码静默回绕（C 风格）：
`INT_MAX + 1 == INT_MIN`。没有 SafeInt 式的检查。锁定测试：
`tests/e2e/int_overflow_wrap.n`。

**数值提升**：算术运算遵循对称的 C 风格提升——两个
操作数先提升为较宽的类型，再运算：
- `int + int` → int
- `int + float` / `float + int` → float（两个操作数都提升为 float）
- `float + float` → float

所以 `1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；
赋给更窄的类型（如 `int r = 1.5 + 1;`）会隐式截断。

`string + string`（仅 OP_Add）是拼接。`string - string` 等是编译
错误。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

返回 1（真）或 0（假）。字符串相等比较内容。字符串关系排序
（`<`、`>`、`<=`、`>=`）用 C `strcmp` 式的逐字节比较（例如
`"Z" < "a"` 为真，因为 `'Z'`（90）< `'a'`（97））。字符串是 UTF-8，
而 UTF-8 字节顺序等于码点序，因此对非 ASCII 文本排序同样正确：
`"é" > "z"` 为真。

**比较操作数是带类型的**：

- string 与非 string 混用是**编译错误**（`"a" < 5`、`5 == "a"`）。
  唯一例外是 null 字面量：`s == null` / `c == null` 与 null 哨兵
  比较——对 class/引用操作数是恒等比较；对 string 操作数，null 一侧
  读作空串（句柄 0 是保留的 null 哨兵；真正的空串有自己独立的对象，
  与 null 位型不同）。因此 `"" == null` 比较相等，任何非空串与 null
  不等。
- int/float 对适用与算术相同的对称提升：`-2 < -1.5`
  提升为 float 且为真；`1 == 1.0` 为真。
- class/引用相等（`==`、`!=`）是恒等（同一个堆对象）。
- 带 null 操作数的算术/拼接是编译错误——null 只经上述比较恒等路径
  才有值。

### 逻辑

```nlang
a && b   a || b   !a
```

短路，C 风格：`&&` 仅当 `a` 非零时求值 `b`；`||` 仅当 `a` 为零时
求值 `b`。被跳过的操作数完全不产生可观察效果——没有调用，没有抛错。
结果恒为 `int` 的 `0` 或 `1`（从不是原始操作数值）。`&&`、`||` 与
`!` 的操作数必须是 `int`——与 `if`/`while` 条件同一规则（比较已经
产生 `int`）。

### 成员访问

```nlang
obj.field          // 字段读
obj.field = value  // 字段写
obj.method(args)   // 方法调用
```

对 class 对象，`obj` 必须非 null（运行期 null 检查）。

### 对象创建

```nlang
Node n = new Node();
Node n = new Node(42);
```

在堆上分配，存在构造函数则调用之。

### 类型强制转换

```nlang
int x = 5;
float y = (float)x;
int z = (int)y;
```

int 与 float 之间的显式强制转换。某些上下文允许隐式加宽
（int→float）。

### 基本类型 → string 强制转换

当基本类型（int 或 float）出现在期望 string 的上下文时，NLang 自动
将其强制转换成十进制字符串形式。最常见于字符串拼接，但直接赋值与
字段存储同样触发。

```nlang
string s1 = "x" + 5;       // "x5" —— int 强制转换成 "5"
string s2 = 5 + "x";       // "5x" —— 对称
string s3 = "x=" + 2.5;    // "x=2.5" —— float 用 %g 格式
string s4 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
string s5 = 42;            // "42" —— 直接赋值路径
string s6 = "x" + (-7);    // "x-7" —— 负数带符号格式化
```

**实现**：
- `int → string`：`OP_Int32_to_str`（十进制，经 `std::to_string`）
- `float → string`：`OP_Float_to_str`（`%g` 格式——`2.5` 而非
  `2.500000`）
- 两者都把格式化后的字符串铸造为运行期字符串对象，把新句柄写入
  结果槽位；随后 `OP_Assign` 把结果移入目标槽位。
- `string → int/float` 仍被拒绝——
  请改用标准库的 `s.toInt()` / `s.toFloat()`（见「标准库」）。

**Object.toString() 协议**：

所有 class 实例从 `Object` 继承 `string toString()`。默认实现返回
`"ClassName@heapIdxHex"`（如 `"Point@7"`、`"Point@ff"`）。用户类
声明 `string toString() { ... }` 即可覆写——按名虚分派，与
`equals`/`getHashCode` 相同。

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

分派矩阵：

| 接收者 | `.toString()` 结果 | 是否覆写？ |
|----------|---------------------|-----------|
| class（用户覆写） | 用户定义 | 是 |
| class（无覆写） | `"ClassName@hex(heapIdx)"` | 否（Object 内建） |
| enum | 枚举成员名（如 `"Red"`） | 否 |
| int | 十进制字符串（如 `"42"`） | 否 |
| float | `%g` 格式（如 `"2.5"`） | 否 |
| string | 自身（恒等） | 否 |

**隐式强制转换**：`"x" + obj` 自动调用 `obj.toString()`，与 Java/C#
相同。适用于 class、enum、int 与 float 接收者。struct 接收者**永久
排除**——`"x" + structInstance` 是编译错误（struct 在 NLang 中是纯
数据类型；需要对象语义请用 class）。

**枚举名输出**：`Color.Red.toString()` 返回 `"Red"`（不是 `"0"`）。
编译器内嵌每个枚举一张名字表；VM 用 `OP_Enum_to_str` 按值查成员名。
越界的枚举值在运行期抛错。

**字符串恒等**：`"hello".toString()` 返回 `"hello"`——编译器把它
折叠为无操作（不发射指令）。

**限制**：
- 隐式强制转换发生时没有警告（静默，与 Java 相同）
- `struct.toString()` / `"x" + structInstance` ——永久拒绝

**转义序列**（双引号字面量内）：

| 转义      | 产生              |
|-------------|-----------------------|
| `\n` `\r` `\t` | 换行、回车、制表   |
| `\\` `\"` `\'` | 反斜杠、双引号、单引号 |
| `\0` `\a` `\b` `\f` `\v` | NUL、响铃、退格、换页、垂直制表 |
| `\x`/`\u`... | 不支持        |

其他转义（如 `\q`）是编译错误——转义序列从不按字面反斜杠对透传。
插值与转义可组合：`"${name}\n"` 先插值再追加换行。


### 字符串插值

```nlang
string name = "world";
string s = "Hello ${name}!";   // "Hello world!"
```

NLang 在双引号字符串字面量内支持 `${identifier}` 插值——具名变量的
值经与「基本类型 → string」和「集合 `toString()`」相同的强制转换
路径渲染。插值在编译期被改写为等价的 `OP_Add` 字符串拼接表达式；
不引入任何新指令。

**语法约束**：

- `${...}` 内只支持单个标识符。`${a + b}`、`${obj.method()}`、
  `${this.x}` 之类的复杂表达式在解析阶段被拒绝。请改用单独变量：
  `int sum = a + b; "result=${sum}"`。
- `${name}` 中 `name` 不在作用域内时产生编译错误
  （"undefined identifier"）——与其他任何未定义标识符引用同一路径。
- 空 `${}` 与非法标识符内容（如 `${123}`、`${a b}`）产生编译错误。

**美元转义**：`$$` 产生字面 `$`。`$${name}` 产生字面文本 `${name}`
（不插值）。后面不接 `$` 或 `{` 的孤立 `$` 按字面 `$` 保留。

```nlang
string name = "x";
string a = "$${name}";  // 字面 "${name}"
string b = "price: $";  // 字面 "price: $"
string c = "$$100";     // 字面 "$100"
```

**类型分派**：标识符解析出的类型决定自动施加的强制转换：

| 标识符类型 | 施加的强制转换 |
|-----------------|------------------|
| `int` | `OP_Int32_to_str` |
| `float` | `OP_Float_to_str` |
| `string` | 无 |
| `enum` | `OP_Enum_to_str` |
| `Array` | `OP_Array_to_str` |
| `List` / `Dict` | `OP_CallMethod "toString"` |
| `class` | `OP_CallMethod "toString"` |

### 运行期检查的转换（`as`）

```nlang
expr as TypeName
```

支持以下运行期检查的转换：

- **拆箱**：`o as int` / `o as float` / `o as string`——拆开装箱的
  基本类型值。`o` 为 null 或装箱类型标签不匹配时抛错。
- **class 向下转换**：`o as SubClass`——验证 `o` 的运行期类是
  `SubClass` 或其子类。不匹配抛错。
- **恒等 / 向上转换**：`o as Object`——无操作（任何 class 已经是
  Object）。为对称性而允许。

类型不兼容的转换（`5 as string`；`o` 持有 class 引用时的
`o as int`）是编译错误——`as` 只允许 same/box/unbox/downcast。
数组操作数被整体拒绝（`ia as int`、`ia as Object`——"the cast
operand is an array"）：数组值的合法转换是它自身的数组类型与全部
位置上的 string 目标——从不经 `as`（完整的数组值转换规则见
[已知限制](known-limitations.md)）。

### 集合初始化器

NLang 为数组、列表、dict 与聚合（struct/class）初始化提供 C 风格的
集合字面量。两种语法形式：

**裸方括号形式 `[...]`**——只允许在左值或赋值目标能让编译器推断
出集合类型的位置使用。适用于数组（`T[]`）与 `List<T>`：

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [new Point{x:1, y:2}, new Point{x:3, y:4}];
```

**显式形式 `new Type{...}`**——可用于任何表达式位置（函数实参、
返回值、独立表达式）。dict、struct 与 class 初始化必须用它，因为
裸 `{...}` 会与块语句（`{...}` 包围的语句组）文法冲突：

```nlang
Dict<string, int> d = new Dict<string, int>{"a":1, "b":2};
Point p = new Point{x:1, y:2};
List<int> lst = new List<int>{1, 2, 3};
return new Point{x:0, y:0};
foo(new Point{x:1, y:2}, new Point{x:3, y:4});
```

**`{...}` 内的条目形式：**

- `字符串字面量 : Expression`——dict 条目（string 键）
- `标识符 : Expression`——struct/class 字段（如 `x:1, y:2`）
- `Expression`（无键）——list 元素（仅当 Type 为 `List<T>` 时合法）

**类型消歧**：编译器用左值变量（或 `new Type{...}` 中的显式
`Type`）挑选种类：

| 目标类型          | 形式    | 条目种类              |
|----------------------|---------|-------------------------|
| `T[]`（数组）        | `[...]` | 仅值              |
| `List<T>`            | `[...]` 或 `new List<T>{...}` | 仅值 |
| `Dict<K,V>`          | `new Dict<K,V>{...}` | `key : value`（string 键） |
| struct               | `new StructName{...}` | `field : value`（标识符键） |
| class                | `new ClassName{...}` | `field : value`（标识符键） |

**class 初始化要求**：class 必须有无参构造函数（显式或隐式）。代码
生成把 `new C{f1:v1, ...}` 降级为 `new C()` 后接逐字段的
`OP_StoreField` 赋值。

**递归嵌套**：初始化列表可以包含其他初始化列表。嵌套泛型元素类型
（`List<List<int>>`、`Dict<K, List<V>>`）受支持
（见上文 `List<T>` 下的「嵌套泛型」）。

**空集合**：裸 `[]` 不支持（词法分析器把 `[]` 匹配为单个
记号，供数组类型后缀语法使用）。请用显式空形式：
`new List<T>{}`、`new Dict<K,V>{}`，数组用 `new int[0]`。

**函数实参消歧**：裸 `[...]` 作函数实参目前不支持——编译器无法
在无重载决议的情况下推断目标类型，因此产生编译错误。函数实参请用
显式 `new Type{...}` 形式。

**初始化期间的改动是未定义行为。**条目自左向右求值并按序赋值；在
条目表达式内读取半成品集合（如 `[1, foo(arr)]`，其中 `foo` 读
`arr`）是未定义行为。抛异常的条目会让集合停留在半构造状态。
