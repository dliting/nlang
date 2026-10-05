# 类型语义


NLang中每种类型都对四个操作定义了语义：**赋值**（`a = b`）、**参数传递**、**返回值**，以及作为**字段或数组元素**的用法。本页是完整的逐类型摘要；每行链接到详细讲解该类型的页面。

### 一览表

| 类型        | 种类              | 赋值           | 参数传递      | 返回值          | 作为字段/元素   |
|-------------|-------------------|----------------|--------------|----------------|----------------|
| `byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong` | 值 | 拷贝 | 拷贝 | 拷贝 | 拷贝 |
| `float` `double` | 值             | 拷贝           | 拷贝         | 拷贝           | 拷贝           |
| `bool` `char` | 值              | 拷贝           | 拷贝         | 拷贝           | 拷贝           |
| `string`    | 值（不可变）      | 拷贝句柄       | 拷贝句柄     | 拷贝句柄       | 拷贝句柄       |
| `enum`      | 值（int32）       | 拷贝           | 拷贝         | 拷贝           | 拷贝           |
| `struct`    | 值（深拷贝）      | 深拷贝         | 深拷贝       | 深拷贝         | 深拷贝（自有） |
| `class`     | 引用              | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `interface` | 引用              | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `T[]`       | 引用（堆上）      | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `List<T>`   | 引用              | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `Dict<K,V>` | 引用              | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `Object`    | 引用（装箱）      | 拷贝引用       | 传递引用     | 返回引用       | 存引用         |
| `Func`      | 值（函数引用）    | 拷贝           | 拷贝         | 拷贝           | 拷贝           |

### 逐类型说明

- **12个标量基本类型**（整型家族、`float`/`double`、`bool`、`char`）—— 值类型。赋值、传参、返回都按值；数值家族适用算术提升与转换矩阵（见下方）。见[基本类型](primitives.md)。
- **`string`** —— 经不可变驻留对象实现*值*语义：句柄被拷贝，但对象内容永不改变，所以共享句柄无害。`==`比较内容而非身份。null字符串句柄（0）读作空串`""`。见[字符串](string.md)。
- **`enum`** —— 以int32为底座的值类型。按值赋值、传参、返回；按整数值比较；没有独立对象身份。见[枚举](enum.md)。
- **`struct`** —— 具有**深拷贝**语义的值类型：拷贝时复制整个聚合，包括嵌套struct字段。唯一例外是struct内class类型字段，浅拷贝（共享引用）。见[结构体](struct.md)。
- **class** —— 引用类型。赋值/传参/返回拷贝引用（堆索引）；两个名字指向同一对象。null引用在成员访问时抛`NullPointerException`。见[类](class.md)。
- **`interface`** —— 与class类似的引用类型；接口类型的值指向实现对象。见[接口](interface.md)。
- **`T[]`** —— 引用类型：数组在堆上；赋值与传参拷贝数组*引用*而非元素。见[数组](array.md)。
- **`List<T>` / `Dict<K,V>`** —— 引用类型；赋值与传参拷贝容器引用。见[内建泛型类](builtin-generic-classes.md)。
- **`Object`** —— 可持有装箱基本类型或class引用的引用类型；赋值拷贝引用。见[Object与装箱](object.md)。
- **`Func`** —— 一等值：函数引用按值拷贝。见[函数](functions.md)。

### 数值转换矩阵

数值类型之间的转换分三档：**隐式**（编译器自动插入）、**有损警告**（隐式但警告）、**显式`as`**（用户明示，不警告）。完整规则与示例见[基本类型](primitives.md)与[类型强制转换](type-casts.md)。

**隐式（无警告）**：

- 同符号秩加宽：`byte→short→int→long`；`ubyte→ushort→uint→ulong`。
- 跨符号值域包含：`ubyte→short/int/long`；`ushort→int/long`；`uint→long`。
- 整型→浮点且目标能精确表示该值域：窄整型（byte..ushort）→ `float/double`、`int/uint` → 仅`double`。
- 浮点加宽：`float→double`。
- `char→string`（编码为该码点的Unicode转换格式（UTF-8，Unicode Transformation Format）串）；`enum→int`（底座）。

**隐式但有损警告**——目标类型装不下源值域时，编译器发出`implicit conversion from 'X' to 'Y' loses precision`（`ncc --no-warn`可整体抑制）：

- `int/uint/long/ulong → float`
- `long/ulong → double`

常量操作数的值精确可表示时豁免（`float f = 5;`无警告）——警告指向「本次转换的实际损失」而非「类型对的可能损失」。

**常量适配特例**：整型**字面量**赋给更窄目标，值在目标值域内即隐式合法（`byte b = 5;`）；只覆盖字面量本身，不覆盖折叠表达式（`byte b = 1 + 2;`仍需`as`）。double字面量到`float`目标同例。见[基本类型](primitives.md)。

**显式`as`**：一切收窄（`float→int`、`double→float`、`long→int`、`ulong→long`…）、反向跨符号（`short→ubyte`…）、`数值↔char`。见[类型强制转换](type-casts.md)。

**禁止**：`bool`与任何类型互转；`string→数值`的`as`（用`toInt()`家族）。

**算术提升**：窄于int的整型操作数先提升到int；结果类型是能同时隐式接收两个操作数的最小类型（`int + uint` → `long`；`int`/`long`与`ulong`混合是编译错误）；浮点参与取较大浮秩。bool与char不参与算术。见[运算符](operators.md)。

### 为什么分两族

分界是值vs引用：

- **值类型**（12个标量基本类型、`enum`、`string`、`struct`、`Func`）在赋值时拷贝。两个变量持有独立数据；修改一个绝不影响另一个。（`string`虽是对象却属值类型，因为对象不可变。）
- **引用类型**（`class`、`interface`、数组、`List`、`Dict`、`Object`）共享底层对象。赋值拷贝引用，因此两个名字观察同一对象、同一份改动。

引用族共享一条规则：**null引用在成员访问时抛`NullPointerException`**（class、接口、数组、容器的null行为一致）。null语义见[类](class.md)。
