# 基本类型


NLang有12个标量基本类型：8个整型、2个浮点型、`bool`与`char`。第13个基本类型`string`是不可变对象，有独立页面：[字符串](string.md)。

| 类型 | 大小 | 语义 | 字面量 |
|------|------|------|--------|
| `byte` / `ubyte` | 1字节 | s8 / u8 | 值域升档（见下） |
| `short` / `ushort` | 2字节 | s16 / u16 | 值域升档 |
| `int` / `uint` | 4字节 | s32 / u32 | 值域升档 |
| `long` / `ulong` | 8字节 | s64 / u64 | 值域升档 |
| `float` | 4字节 | IEEE 754单精度 | `1.5f`（必须带`f`后缀） |
| `double` | 8字节 | IEEE 754双精度 | `1.5`、`2.5e-3` |
| `bool` | 4字节 | 真值 | `true` / `false` |
| `char` | 4字节 | Unicode标量值（排除代理区） | `'a'`、`'é'`、`'\n'`、`'\u0041'` |

### 数字字面量

**整数字面量按值域升档**：无后缀十进制整数字面量的类型是能容纳它的最窄类型——落在int32值域内是`int`；超过int32但在int64内是`long`；超过INT64_MAX的正数是`ulong`（INT64_MAX为C记法，NLang无此常量）；再超是编译错误。十六进制字面量同规则（`0xFF`是`int` 255）。字面量**不带前置符号**——`-5`是对字面量`5`的一元负号运算，`-9223372036854775808`因此合法（按ulong字面量取负后落回long值域）。

**浮点字面量**：无后缀小数与指数形式都是`double`（`1.5`、`2.5e-3`、`1e5`）；`float`必须带`f`后缀（`1.5f`）。

**常量适配**（Java/C#同款规则）：整型**字面量**（含一元负号形式）赋给更窄的目标类型时，值在目标值域内即隐式合法——`byte b = 5;`合法，`byte b = 1000;`是编译错误`constant 1000 out of range for 'byte'`。该特例只覆盖字面量本身，不覆盖折叠表达式——`byte b = 1 + 2;`仍需显式`as byte`（`1 + 2`的类型已是`int`）。同一特例覆盖double字面量到`float`目标：`float f = 1.5;`合法，`float f = 1e50;`编译错误；整数目标不接受浮点常量（`int x = 2e5;`编译错误）。

### bool

`bool`只有`true` / `false`两个值。**生产者**：全部比较运算（`==` `!=` `<` `>` `<=` `>=`）、逻辑运算`&&`/`||`/`!`、标准库全部谓词（`fs.exists`、`s.startsWith`、`List.contains`、`Dict.containsKey`等）。**消费者**：五处条件位置——`if`/`while`/`do-while`/`for`的条件与`assert`的实参——以及`&&`/`||`/`!`的操作数，都**只接受bool**；写`if (1)`是编译错误（`if condition must be bool, not "Int32"`）。bool不与任何其他类型相互转换，也不参与算术。

### char

`char`持有一个Unicode标量值（码点，排除U+D800..U+DFFF代理区）。字面量用单引号：普通字符`'a'`、非美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）字符`'é'`、转义（`'\n'`、`'\r'`、`'\t'`、`'\''`、`'\\'`五个）、`\uXXXX`转义`'\u0041'`（即`'A'`）。char字面量里的代理区转义是编译错误；string字面量里相邻的代理区转义按码点组合（`"\ud83d\ude00"`即U+1F600，Java同款）。

char与数值之间**不经隐式转换**：

- `char`参与算术（`c + 1`）是编译错误——先`c as int`取码点。
- char只与char比较（`'a' < 'b'`合法，产出bool）；与数值比较需先`as int`。
- `65 as char`显式构造（运行期校验标量值合法性，非法值抛运行期错误）；`c as int`反向取码点。

char与string的桥接（`"x" + 'y'`拼接、`foreach (char c in s)`码点迭代、`charAt`/`charCount`方法）见[字符串](string.md)。

### 表示链：从源码到控制台

一个表示问题贯穿整条链路——char在源文件里、编译后、控制台上各是什么形式：

- **源文件必须是Unicode转换格式（UTF-8，Unicode Transformation Format）。**词法器把字符字面量的1-4字节UTF-8序列解码为一个码点（`'é'`是2个字节`C3 A9`，解码为U+00E9）。整个文件在词法前经过严格UTF-8校验：无效字节被明确拒绝（`Source file is not valid UTF-8 ... (first invalid
  byte at line N)`），UTF-16保存的文件得到专门提示——无效编码字节不会混入字符串常量。文件开头的UTF-8字节顺序标记（BOM，byte order mark）被接受并跳过（编辑器的「UTF-8 with BOM」保存形式可用）；UTF-16等其他编码得到上述专门提示。
- **编译后char就是它的32位码点——既不是UTF-8也不是UTF-16。**每个char存于4字节槽位，内容即原始码点：帧槽、char字面量的4字节立即数、装箱记录与流写入里的4字节值皆如此。模块格式用一个字节标记类型（char为kind 19），值本身端到端都是朴素码点。
- **UTF-8在string一侧**：string对象是UTF-8字节序列（见[字符串](string.md)「编码与长度」节）。char加入string时转成该形式——`"x" + 'y'`把码点编码为1-4个UTF-8字节。反方向上，`s.charAt(i)`从字节i处解码码点，`"65".toChar()`解析十进制码点。
- **控制台收到的是UTF-8字节。**`io.print`把char以其UTF-8编码、把string以其原始字节原样写入标准输出。命令行工具启动时把所在控制台切换到UTF-8代码页，Windows默认控制台即可正常显示非ASCII输出（重定向到文件/管道时字节原样，仍是UTF-8）。

### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除零/模零抛运行期错误——浮点除零也不例外，抛错而不是产生IEEE 754的±inf/NaN（NLang在这点上不同于C/C++/Java）。`%`对浮点操作数取fmod余数（`7.5 % 2.0` = 1.5）。

**整数溢出**按二进制补码静默回绕（C风格）：INT_MAX + 1 == INT_MIN。没有SafeInt式的检查。

**数值提升**：窄于int的整型操作数先提升为int（`byte + byte`的结果是`int`）；随后结果类型是**能同时隐式接收两个操作数的最小类型**：

- `int + int` → `int`；`byte + byte` → `int`
- `int + uint` → `long`（int32与u32都装进s64）
- `long + uint` → `long`
- `int`/`long`与`ulong`混合 → **编译错误**（`no implicit common type`），先显式统一符号域
- 浮点参与时取较大的浮秩：`int + float` / `float + int` → `float`（对称）；任一操作数为`double` → `double`

所以`1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；赋回更窄的类型需要显式`as`（常量适配特例除外，见上）。`string + string`是拼接（见[字符串](string.md)）；`string - string`与其他算术形式是编译错误。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

**比较结果一律是`bool`。**数值操作数适用与算术相同的提升规则：`-2 < -1.5`提升为double且为真；`1 == 1.0`为真。char只与char比较。所有类型（string、class、null）的完整比较规则在[运算符](operators.md)「比较」节。

### 强制转换

整型家族、浮点、bool、char之间的转换矩阵——哪些隐式、哪些必须显式`as`、哪些禁止——见[类型强制转换](type-casts.md)。
