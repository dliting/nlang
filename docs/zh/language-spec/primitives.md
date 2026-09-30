# 基本类型


NLang 有 12 个标量基本类型：8 个整型、2 个浮点型、`bool` 与 `char`。
第 13 个基本类型 `string` 是不可变对象，有独立页面：[字符串](string.md)。

| 类型 | 大小 | 语义 | 字面量 |
|------|------|------|--------|
| `byte` / `ubyte` | 1 字节 | s8 / u8 | 值域升档（见下） |
| `short` / `ushort` | 2 字节 | s16 / u16 | 值域升档 |
| `int` / `uint` | 4 字节 | s32 / u32 | 值域升档 |
| `long` / `ulong` | 8 字节 | s64 / u64 | 值域升档 |
| `float` | 4 字节 | IEEE 754 单精度 | `1.5f`（必须带 `f` 后缀） |
| `double` | 8 字节 | IEEE 754 双精度 | `1.5`、`2.5e-3` |
| `bool` | 4 字节 | 真值 | `true` / `false` |
| `char` | 4 字节 | Unicode 标量值（排除代理区） | `'a'`、`'中'`、`'\n'`、`'\u0041'` |

### 数字字面量

**整数字面量按值域升档**：无后缀十进制整数字面量的类型是能容纳它的最窄
类型——落在 int32 值域内是 `int`；超过 int32 但在 int64 内是 `long`；超过
INT64_MAX 的正数是 `ulong`；再超是编译错误。十六进制字面量同规则
（`0xFF` 是 `int` 255）。字面量**不带前置符号**——`-5` 是对字面量 `5` 的
一元负号运算，`-9223372036854775808` 因此合法（按 ulong 字面量取负后落回
long 值域）。

**浮点字面量**：无后缀小数与指数形式都是 `double`（`1.5`、`2.5e-3`、
`1e5`）；`float` 必须带 `f` 后缀（`1.5f`）。

**常量适配**（Java/C# 同款规则）：整型**字面量**（含一元负号形式）赋给
更窄的目标类型时，值在目标值域内即隐式合法——`byte b = 5;` 合法，
`byte b = 1000;` 是编译错误 `constant 1000 out of range for 'byte'`。
该特例只覆盖字面量本身，不覆盖折叠表达式——`byte b = 1 + 2;` 仍需显式
`as byte`（`1 + 2` 的类型已是 `int`）。同一特例覆盖 double 字面量到
`float` 目标：`float f = 1.5;` 合法，`float f = 1e50;` 编译错误；整数
目标不接受浮点常量（`int x = 2e5;` 编译错误）。

### bool

`bool` 只有 `true` / `false` 两个值。**生产者**：全部比较运算
（`==` `!=` `<` `>` `<=` `>=`）、逻辑运算 `&&`/`||`/`!`、标准库全部谓词
（`fs.exists`、`s.startsWith`、`List.contains`、`Dict.containsKey` 等）。
**消费者**：五处条件位置——`if`/`while`/`do-while`/`for` 的条件与
`assert` 的实参——以及 `&&`/`||`/`!` 的操作数，都**只接受 bool**；
写 `if (1)` 是编译错误（`if condition must be bool, not "Int32"`）。
bool 不与任何其他类型相互转换，也不参与算术。

### char

`char` 持有一个 Unicode 标量值（码点，排除 U+D800..U+DFFF 代理区）。
字面量用单引号：普通字符 `'a'`、非 ASCII 字符 `'中'`、转义
（`'\n'`、`'\r'`、`'\t'`、`'\''`、`'\\'` 五个）、`\uXXXX` 转义
`'\u0041'`（即 `'A'`）。char 字面量里的代理区转义是编译
错误；string 字面量里相邻的代理区转义按码点组合（`"😀"` 即
U+1F600 😀，Java 同款）。

char 与数值之间**不经隐式转换**：

- `char` 参与算术（`c + 1`）是编译错误——先 `c as int` 取码点。
- char 只与 char 比较（`'a' < 'b'` 合法，产出 bool）；与数值比较需先
  `as int`。
- `65 as char` 显式构造（运行期校验标量值合法性，非法值抛运行期错误）；
  `c as int` 反向取码点。

char 与 string 的桥接（`"x" + 'y'` 拼接、`foreach (char c in s)` 码点迭代、
`charAt`/`charCount` 方法）见 [字符串](string.md)。

### 表示链：从源码到控制台

一个表示问题贯穿整条链路——char 在源文件里、编译后、控制台上
各是什么形式：

- **源文件是 UTF-8。** 词法器把字符字面量的 1-4 字节 UTF-8
  序列解码为一个码点（`'中'` 是 3 个字节 `E4 B8 AD`，解码为
  U+4E2D）。文件不得以 UTF-8 字节序标记（BOM）开头：BOM 不被
  识别，会静默破坏首个词法记号（带 BOM 的 `int main()...`
  「编译成功」但没有 main 函数）。源码请保存为**不带 BOM 的
  UTF-8**。
- **编译后 char 就是它的 32 位码点——既不是 UTF-8 也不是
  UTF-16。** 每个 char 存于 4 字节槽位，内容即原始码点：帧槽、
  char 字面量的 4 字节立即数、装箱记录与流写入里的 4 字节值皆
  如此。模块格式用一个字节标记类型（char 为 kind 19），值本身
  端到端都是朴素码点。
- **UTF-8 在 string 一侧**：string 对象是 UTF-8 字节序列（见
  [字符串](string.md)「编码与长度」节）。char 加入 string 时转成
  该形式——`"x" + 'y'` 把码点编码为 1-4 个 UTF-8 字节。反方向上，
  `s.charAt(i)` 从字节 i 处解码码点，`"65".toChar()` 解析十进制
  码点。
- **控制台收到的是 UTF-8 字节。** `io.print` 把 char 以其 UTF-8
  编码、把 string 以其原始字节原样写入标准输出。工具链不切换
  控制台代码页，因此 Windows 上非 ASCII 输出需要 UTF-8 终端——
  Windows Terminal，或传统控制台里先 `chcp 65001`。

### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除零/模零抛运行期错误——浮点除零也不例外，抛错而不是
产生 IEEE 754 的 ±inf/NaN（NLang 在这点上不同于 C/C++/Java）。`%` 对
浮点操作数取 fmod 余数（`7.5 % 2.0` = 1.5）。

**整数溢出**按二进制补码静默回绕（C 风格）：`INT_MAX + 1 == INT_MIN`。没有
SafeInt 式的检查。锁定测试：`tests/e2e/int_overflow_wrap.n`。

**数值提升**：窄于 int 的整型操作数先提升为 int（`byte + byte` 的结果是
`int`）；随后结果类型是**能同时隐式接收两个操作数的最小类型**：

- `int + int` → `int`；`byte + byte` → `int`
- `int + uint` → `long`（int32 与 u32 都装进 s64）
- `long + uint` → `long`
- `int`/`long` 与 `ulong` 混合 → **编译错误**（`no implicit common type`），
  先显式统一符号域
- 浮点参与时取较大的浮秩：`int + float` / `float + int` → `float`（对称）；
  任一操作数为 `double` → `double`

所以 `1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；赋回更窄
的类型需要显式 `as`（常量适配特例除外，见上）。`string + string` 是拼接
（见 [字符串](string.md)）；`string - string` 与其他算术形式是编译错误。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

**比较结果一律是 `bool`。** 数值操作数适用与算术相同的提升规则：
`-2 < -1.5` 提升为 double 且为真；`1 == 1.0` 为真。char 只与 char 比较。
所有类型（string、class、null）的完整比较规则在
[运算符](operators.md)「比较」节。

### 强制转换

整型家族、浮点、bool、char 之间的转换矩阵——哪些隐式、哪些必须显式
`as`、哪些禁止——见 [类型强制转换](type-casts.md)。
