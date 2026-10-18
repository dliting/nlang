# 运算符


### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除以零（包括取模）抛运行期错误——这包括浮点除零，它抛出而不是产生IEEE 754的±inf/NaN（NLang在此处与C/C++/Java不同）。`%`对浮点操作数取fmod余数（`7.5 % 2.0` = 1.5）。

**整数溢出**以二进制补码静默回绕（C风格）：`INT_MAX + 1 == INT_MIN`。没有SafeInt式的检查。

**数值提升**：窄于int的整型操作数先提升为int；随后结果类型是能同时隐式接收两个操作数的最小类型：

- `int + int` → `int`；`byte + byte` → `int`
- `int + uint` → `long`；`long + uint` → `long`
- `int`/`long`与`ulong`混合 → 编译错误（先显式统一符号域）
- `int + float` / `float + int` → `float`（对称）；任一操作数为`double`
  → `double`

所以`1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；赋回更窄的类型需要显式`as`（常量适配特例除外）。bool与char不参与算术。完整的提升与转换规则见[类型语义](type-semantics.md)。

`string + string`（仅OP_Add）是拼接；`"x" + 'y'`（char隐式转string）与`+`标量操作数同样拼接。`string - string`等是编译错误。见[字符串](string.md)。

### 位移

```nlang
a << b    a >> b
```

按位左移与右移。**左操作数决定结果类型**（Java/C#规则）：窄于int的整型左操作数先单目提升为int（`byte << 2`是`int`）。右操作数（计数）按普通隐式转换规则进入结果类型（见[类型语义](type-semantics.md)）：int计数适用于int与long载体，但不适用于uint或ulong；long计数只适用于long；能装进载体的常量计数对任何载体都合法。其余情形是编译错误，诊断会点名载体与计数两者的类型（`1 << long计数`）。enum成员按其int32载体参与（`Color.Green << 1 == 2`）。float、double、bool、char、string操作数是编译错误（`operator '<<' requires integer operands`）；null操作数报`null is not a valid shift operand`。

有符号行`>>`是算术右移（符号扩展，`-8 >> 1 == -4`）；无符号行是逻辑右移（零填充）。`<<`按二进制补码回绕（`1 << 31`在int上是`INT_MIN`）。计数必须在`[0, 位数)`内——负数或达到类型位宽的计数抛运行期错误（与除零同类），不做C的未定义行为，也不做Java的掩码回绕。

优先级介于关系与加减之间：加法先于位移（`1 << 2 + 3`是`1 << 5`），位移先于关系（`1 << 2 < 8`比较的是`4 < 8`）。没有`<<=`/`>>=`复合赋值形式。

泛型实参列表内的`>>`仍是嵌套类型的收尾（`List<List<int>>`）；只有出现在表达式位置时它才是位移运算符，两者不会冲突。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

比较结果一律是**`bool`**（`true` / `false`）。

**比较操作数是有类型的**：

- **string/非string混合是编译错误**（`"a" < 5`、`5 == "a"`）。唯一例外是null字面量：`s == null` / `c == null`与null哨兵比较——对class/引用操作数是同一性；对string操作数，null侧读作空字符串（句柄0是保留的null哨兵；真正的空字符串有自己的对象，按位与null不同）。因此`"" == null`比较相等，任何非空字符串比较不等。
- **数值对**得到与算术相同的对称提升：`-2 < -1.5`提升为double且为真；`1 == 1.0`为真。见[基本类型](primitives.md)。
- **char只与char比较**（`'a' < 'b'`合法）；char与数值比较不经隐式转换，先`c as int`取码点（`a char value can only be compared with
  a char value`）。
- **字符串相等**按内容比较；字符串关系序用C `strcmp`式的逐字节比较（例如`"Z" < "a"`为真，因为`'Z'`(90) < `'a'`(97)）。因为字符串以Unicode转换格式（UTF-8，Unicode Transformation Format）存储且UTF-8字节序等于码点序，对非美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）文本排序也正确：`"é" > "z"`为真。见[字符串](string.md)「比较」。
- **class/引用相等**（`==`、`!=`）是同一性（同一堆对象）。要按值比较，用`equals`方法并在自己的类里覆写它——`==`不受覆写影响；见[Object与装箱](object.md)「`equals`与`getHashCode`」与[类](class.md)「虚方法与覆写」。
- **带null操作数的算术/拼接**是编译错误——null只有通过上面的比较同一性路径才有值。

### 逻辑

```nlang
a && b   a || b   !a
```

短路，C风格：`&&`仅在`a`为`true`时求值`b`；`||`仅在`a`为`false`时求值`b`。被跳过的操作数完全没有可观察效果——没有调用、没有抛出。结果永远是**`bool`**（绝不是原始操作数值）。`&&`、`||`、`!`的操作数必须是`bool`（`operator '&&' requires bool operands`）——比较与谓词已经产生bool。见[语句](statements.md)「条件类型」。
