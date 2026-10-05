# 类型强制转换


在类型之间转换值有两种方式：显式的`as`运算符（标量收窄、数值↔char、拆箱、类向下强转），以及向`string`的隐式强制转换。标量家族内部哪些转换隐式合法（加宽、值域包含、整型→浮点）由转换矩阵决定——见[类型语义](type-semantics.md)。

### 显式强转（`as`）

```nlang
expr as TypeName
```

`as`是NLang唯一的显式转换形式（没有C风格的`(T)expr`前缀强转——前缀形式无法可靠地与括号表达式区分，解析器分不清`(foo) + bar`与`(foo + bar)`；关键字运算符没有这种二义性，与C#、TypeScript、Kotlin一致）。支持四类转换：

- **标量收窄与跨符号**（按位截断，C# unchecked同构）：`d as int`（double→int）、`d as float`（double→float）、`l as int`（long→int）、`s as ubyte`（反向跨符号）——一切隐式矩阵不放行的标量对都可用`as`明示。显式即用户明示，**不产生有损转换警告**。
- **数值↔char**：`c as int`取码点；`65 as char`显式构造char，运行期校验标量值合法性（代理区或超过U+10FFFF抛运行期错误`value is not a valid Unicode scalar value`）。
- **拆箱**：`o as int` / `o as long` / `o as string`等——解开一个装箱基本类型，全部12个标量都可拆。若`o`为null或装箱类型标签不匹配则抛出。
- **类向下强转**：`o as SubClass`——校验`o`的运行期类是`SubClass`或其子类。不匹配则抛出。`o as Object`是无操作（为对称性允许）。

`as`只用于矩阵不放行的转换对——对**已经隐式合法**的转换使用`as`也是编译错误（`` `as` cannot perform implicit conversion `UByte` → `Int32` ``）：`ubyte`到`int`本来就隐式加宽，写`b as int`是冗余，直接`int x = b;`即可。隐式矩阵见[类型语义](type-semantics.md)。

**禁止的`as`**：

- `string → 数值`：`"5" as int`是编译错误（`` Invalid cast: `String as
  Int32` is not allowed ``）——请用标准库的`s.toInt()` / `s.toFloat()` /
  `s.toLong()` / `s.toDouble()`家族（见[标准库](standard-library.md)）。
- `bool ↔ 任何类型`：bool不参与转换。
- 数组操作数被直接拒绝（`ia as int`、`ia as Object`——"the cast operand
  is an array"）：数组值在每个位置的合法转换是其自身数组类型与string目标——绝不用`as`（完整的数组值转换规则见[已知限制](known-limitations.md)）。

unbox/向下强转的Object侧见[Object与装箱](object.md)。

### 基本类型 → string强制转换

当标量基本类型（全部12个）出现在期望string的上下文时，NLang自动把它强转为字符串形式。最常见于字符串拼接，但在直接赋值、字段存储与你自己代码中声明的函数实参处也会触发。**包（库）调用是例外**——库的`string`形参只接受`string`（见[标准库](standard-library.md)）。

```nlang
string s1 = "x" + 5;       // "x5" — int 被强转为 "5"
string s2 = 5 + "x";       // "5x" — 对称
string s3 = "x=" + 2.5;    // "x=2.5" — double 按最短往返格式化
string s4 = "x" + 'y';     // "xy" — char 强转为它的 UTF-8 编码
string s5 = "ok=" + true;  // "ok=true" — bool 强转为 true/false
string s6 = "n=" + 5000000000;   // "n=5000000000" — long 直显
string s7 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
```

**格式化规则**：bool → `true`/`false`；char → 该码点的Unicode转换格式（UTF-8，Unicode Transformation Format）字节（1–4字节）；整型家族按十进制直显（窄整型与long/ulong同样直接）；float与double用**最短往返**格式化——生成最短的可使`parse(format(x)) == x`成立的十进制表示（`0.5`而非`0.500000`；`0.1`打印为`0.1`而非内部的二进制近似）。print、字符串方法与插值共享同一转换点。

**实现**：全部标量→string共用一条泛化指令`OP_Prim_to_str <kind>`（kind立即数选择12个标量行之一的格式化例程）；enum与数组保留各自专属指令。`string → 数值`仍被拒绝——见上方「禁止的as」。

### Object.toString()协议

所有class实例从`Object`继承`string toString()`。默认实现返回`"ClassName@heapIdxHex"`（例如`"Point@7"`、`"Point@ff"`）。用户类通过声明`string toString() { ... }`覆写它——按方法名虚分派，与`equals`/`getHashCode`相同。见[Object与装箱](object.md)。

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

分派矩阵：

| 接收者                | `.toString()`结果              | 可覆写？ |
|-----------------------|---------------------------------|----------|
| class（用户覆写）      | 用户定义                        | 是       |
| class（无覆写）        | `"ClassName@hex(heapIdx)"`      | 否（Object内建） |
| enum                  | enum成员名（例如`"Red"`）      | 否       |
| 全部标量基本类型        | 与 →string强制转换相同的格式化  | 否       |
| string                | 自身（恒等）                    | 否       |

**隐式强制转换**：`"x" + obj`自动调用`obj.toString()`，与Java/C#相同。适用于class、enum与全部标量接收者。struct接收者被**永久排除**——`"x" + structInstance`是编译错误（struct是NLang的纯数据类型；要对象语义请用class）。见[结构体](struct.md)。

**enum名输出**：`Color.Red.toString()`返回`"Red"`（不是`"0"`）。编译器内嵌每张enum名表；VM用`OP_Enum_to_str`按值查找成员名。越界enum值在运行期抛出。见[枚举](enum.md)。

**字符串恒等**：`"hello".toString()`返回`"hello"`——编译器把这折叠为无操作（不发射操作码）。

**限制**：
- 隐式强制转换发生时没有警告（静默，与Java相同）
- `struct.toString()` / `"x" + structInstance`——永久拒绝
