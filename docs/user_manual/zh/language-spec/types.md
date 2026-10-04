# 类型


NLang 是静态类型语言：每个变量、字段、形参与返回值都有声明类型，编译器
按类型检查每个表达式。本节把语言中的每种类型映射到它自己的页面。每页
自包含——如何声明该类型、它的运行期语义，以及它在表达式与语句中的行为。

### 类型一览

| 类型        | 类别                      | 页面 |
|-------------|---------------------------|------|
| `byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong` | 基本类型——整型家族（值类型） | [基本类型](primitives.md) |
| `float` `double` | 基本类型——浮点（值类型） | [基本类型](primitives.md) |
| `bool`      | 基本类型——真值（值类型）  | [基本类型](primitives.md) |
| `char`      | 基本类型——Unicode 标量值（值类型） | [基本类型](primitives.md) |
| `string`    | 基本类型——不可变对象      | [字符串](string.md) |
| `enum`      | 复合类型——值（int32）     | [枚举](enum.md) |
| `struct`    | 复合类型——值（深拷贝）    | [结构体](struct.md) |
| `class`     | 复合类型——引用            | [类](class.md) |
| `interface` | 复合类型——引用            | [接口](interface.md) |
| `T[]`       | 复合类型——引用（堆上）    | [数组](array.md) |
| `List<T>`   | 内建泛型——引用            | [内建泛型类](builtin-generic-classes.md) |
| `Dict<K,V>` | 内建泛型——引用            | [内建泛型类](builtin-generic-classes.md) |
| `Object`    | 引用——装箱                | [Object 与装箱](object.md) |
| `Func<...>` | 一等函数值                | [函数](functions.md) |

### 去哪里查

- **某类型是什么、如何声明** —— 上面的各类型页面。
- **每种类型在赋值、参数传递、返回值、作为字段或数组元素时的行为** ——
  [类型语义](type-semantics.md)，完整的逐类型摘要。
- **值如何在类型之间转换**（隐式加宽、显式 `as`、向 string 的强制转换）——
  [类型强制转换](type-casts.md)。
- **非类型事物的声明**（import、变量、类型别名）—— [声明](declarations.md)。
