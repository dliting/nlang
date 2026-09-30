# 类型强制转换


在类型之间转换值有两种方式：显式的 `as` 运算符（标量收窄、数值↔char、
拆箱、类向下强转），以及向 `string` 的隐式强制转换。标量家族内部哪些
转换隐式合法（加宽、值域包含、整型→浮点）由转换矩阵决定——见
[类型语义](type-semantics.md)。

### 显式强转（`as`）

```nlang
expr as TypeName
```

`as` 是 NLang 唯一的显式转换形式（没有 C 风格的 `(T)expr` 前缀强转——
前缀形式无法可靠地与括号表达式区分，解析器分不清 `(foo) + bar` 与
`(foo + bar)`；关键字运算符没有这种二义性，与 C#、TypeScript、Kotlin
一致）。支持四类转换：

- **标量收窄与跨符号**（按位截断，C# unchecked 同构）：`d as int`
  （double→int）、`d as float`（double→float）、`l as int`（long→int）、
  `s as ubyte`（反向跨符号）——一切隐式矩阵不放行的标量对都可用 `as`
  明示。显式即用户明示，**不产生有损转换警告**。
- **数值↔char**：`c as int` 取码点；`65 as char` 显式构造 char，运行期
  校验标量值合法性（代理区或超过 U+10FFFF 抛运行期错误
  `value is not a valid Unicode scalar value`）。
- **拆箱**：`o as int` / `o as long` / `o as string` 等——解开一个装箱
  基本类型，全部 12 个标量都可拆。若 `o` 为 null 或装箱类型标签不匹配
  则抛出。
- **类向下强转**：`o as SubClass`——校验 `o` 的运行期类是 `SubClass` 或
  其子类。不匹配则抛出。`o as Object` 是无操作（为对称性允许）。

`as` 只用于矩阵不放行的转换对——对**已经隐式合法**的转换使用 `as` 也是编译
错误（`` `as` cannot perform implicit conversion `UByte` → `Int32` ``）：
`ubyte` 到 `int` 本来就隐式加宽，写 `b as int` 是冗余，直接 `int x = b;`
即可。隐式矩阵见 [类型语义](type-semantics.md)。

**禁止的 `as`**：

- `string → 数值`：`"5" as int` 是编译错误（`` Invalid cast: `String as
  Int32` is not allowed ``）——请用标准库的 `s.toInt()` / `s.toFloat()` /
  `s.toLong()` / `s.toDouble()` 家族（见 [标准库](standard-library.md)）。
- `bool ↔ 任何类型`：bool 不参与转换。
- 数组操作数被直接拒绝（`ia as int`、`ia as Object`——"the cast operand
  is an array"）：数组值在每个位置的合法转换是其自身数组类型与 string
  目标——绝不用 `as`（完整的数组值转换规则见
  [已知限制](known-limitations.md)）。

unbox/向下强转的 Object 侧见 [Object 与装箱](object.md)。

### 基本类型 → string 强制转换

当标量基本类型（全部 12 个）出现在期望 string 的上下文时，NLang 自动把
它强转为字符串形式。最常见于字符串拼接，但在直接赋值和字段存储时也会
触发。

```nlang
string s1 = "x" + 5;       // "x5" — int 被强转为 "5"
string s2 = 5 + "x";       // "5x" — 对称
string s3 = "x=" + 2.5;    // "x=2.5" — double 最短往返渲染
string s4 = "x" + 'y';     // "xy" — char 强转为它的 UTF-8 编码
string s5 = "ok=" + true;  // "ok=true" — bool 强转为 true/false
string s6 = "n=" + 5000000000;   // "n=5000000000" — long 直显
string s7 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
```

**渲染规则**：bool → `true`/`false`；char → 该码点的 UTF-8 字节（1–4
字节）；整型家族按十进制直显（窄整型与 long/ulong 同样直接）；float 与
double 用**最短往返**渲染——输出最短的可使 `parse(format(x)) == x` 成立
的十进制表示（`0.5` 而非 `0.500000`；`0.1` 打印为 `0.1` 而非内部的
二进制近似）。print、字符串方法与插值共享同一转换点。

**实现**：全部标量→string 共用一条泛化指令 `OP_Prim_to_str <kind>`（kind
立即数选择 12 个标量行之一的渲染器）；enum 与数组保留各自专属指令。
`string → 数值` 仍被拒绝——见上方「禁止的 as」。

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
| 全部标量基本类型        | 与 →string 强制转换相同的渲染    | 否       |
| string                | 自身（恒等）                    | 否       |

**隐式强制转换**：`"x" + obj` 自动调用 `obj.toString()`，与 Java/C# 相同。适用
于 class、enum 与全部标量接收者。struct 接收者被**永久排除**——
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
