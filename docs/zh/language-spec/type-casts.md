# 类型强制转换


在类型之间转换值有三种方式：C 风格的 `(T)` 强转、运行期检查的 `as` 运算符、
以及向 `string` 的隐式强制转换。

### 显式 `(T)` 强转

```nlang
int x = 5;
float y = (float)x;
int z = (int)y;
```

int 与 float 之间的显式强转。隐式拓宽（int→float）在某些上下文允许（见
[基本类型](primitives.md)）。

### 运行期检查强转（`as`）

```nlang
expr as TypeName
```

支持以下运行期检查转换：

- **拆箱**：`o as int` / `o as float` / `o as string`——解开一个装箱基本
  类型。若 `o` 为 null 或装箱类型标签不匹配则抛出。
- **类向下强转**：`o as SubClass`——校验 `o` 的运行期类是 `SubClass` 或其子类。
  不匹配则抛出。
- **恒等/向上强转**：`o as Object`——无操作（任何类已经是 Object）。为对称性
  允许。

类型不兼容的强转（`o` 持有类引用时的 `5 as string`、`o as int`）是编译错误——
`as` 只允许同型/装箱/拆箱/向下强转。数组操作数被直接拒绝（`ia as int`、
`ia as Object`——"the cast operand is an array"）：数组值在每个位置的合法转换
是其自身数组类型与 string 目标——绝不用 `as`（完整的数组值转换规则见
[已知限制](known-limitations.md)）。

之所以选择 `as` 关键字而非 C 风格 `(T)expr` 前缀强转来做拆箱和类强转，是因为
`(T)expr` 无法可靠地与括号表达式区分（解析器分不清 `(foo) + bar` 与
`(foo + bar)`）。`as` 这类关键字运算符没有这种二义性——这与 C#、TypeScript、
Kotlin 采取的做法一致。unbox/向下强转的 Object 侧见
[Object 与装箱](object.md)。

### 基本类型 → string 强制转换

当基本类型（int 或 float）出现在期望 string 的上下文时，NLang 自动把它强转为
其十进制字符串形式。最常见于字符串拼接，但在直接赋值和字段存储时也会触发。

```nlang
string s1 = "x" + 5;       // "x5" — int 被强转为 "5"
string s2 = 5 + "x";       // "5x" — 对称
string s3 = "x=" + 2.5;    // "x=2.5" — float 用 %g 格式
string s4 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
string s5 = 42;            // "42" — 直接赋值路径
string s6 = "x" + (-7);    // "x-7" — 负数带符号格式化
```

**实现**：
- `int → string`：`OP_Int32_to_str`（十进制，经 `std::to_string`）
- `float → string`：`OP_Float_to_str`（`%g` 格式——`2.5` 而非 `2.500000`）
- 两者都把格式化字符串铸造成一个运行期字符串对象，并把新句柄写入结果槽；
  `OP_Assign` 再把结果移入目标槽。
- `string → int/float` 仍被拒绝——请改用标准库的 `s.toInt()` /
  `s.toFloat()`（见 [标准库](standard-library.md)）。

### Object.toString() 协议

所有 class 实例从 `Object` 继承 `string toString()`。默认实现返回
`"ClassName@heapIdxHex"`（例如 `"Point@7"`、`"Point@ff"`）。用户类通过声明
`string toString() { ... }` 覆写它——按名虚分派，与 `equals`/`getHashCode`
相同。见 [Object 与装箱](object.md)。

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

分派矩阵：

| 接收者                | `.toString()` 结果              | 可覆写？ |
|-----------------------|---------------------------------|----------|
| class（用户覆写）      | 用户定义                        | 是       |
| class（无覆写）        | `"ClassName@hex(heapIdx)"`      | 否（Object 内建） |
| enum                  | enum 成员名（例如 `"Red"`）      | 否       |
| int                   | 十进制字符串（例如 `"42"`）      | 否       |
| float                 | `%g` 格式（例如 `"2.5"`）        | 否       |
| string                | 自身（恒等）                    | 否       |

**隐式强制转换**：`"x" + obj` 自动调用 `obj.toString()`，与 Java/C# 相同。适用
于 class、enum、int、float 接收者。struct 接收者被**永久排除**——
`"x" + structInstance` 是编译错误（struct 是 NLang 的纯数据类型；要对象语义
请用 class）。见 [结构体](struct.md)。

**enum 名输出**：`Color.Red.toString()` 返回 `"Red"`（不是 `"0"`）。编译器内嵌
每张 enum 名表；VM 用 `OP_Enum_to_str` 按值查找成员名。越界 enum 值在运行期
抛出。见 [枚举](enum.md)。

**字符串恒等**：`"hello".toString()` 返回 `"hello"`——编译器把这折叠为无操作
（不发射操作码）。

**限制**：
- 隐式强制转换发生时没有警告（静默，与 Java 相同）
- `struct.toString()` / `"x" + structInstance`——永久拒绝
