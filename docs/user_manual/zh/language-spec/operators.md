# 运算符


### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除以零（包括取模）抛运行期错误——这包括浮点除零，
它抛出而不是产生 IEEE 754 的 ±inf/NaN（NLang 在此处与 C/C++/Java 不同）。
`%` 对浮点操作数取 fmod 余数（`7.5 % 2.0` = 1.5）。

**整数溢出**以二进制补码静默回绕（C 风格）：`INT_MAX + 1 == INT_MIN`。没有
SafeInt 式的检查。锁定测试：`tests/e2e/int_overflow_wrap.n`。

**数值提升**：窄于 int 的整型操作数先提升为 int；随后结果类型是能同时
隐式接收两个操作数的最小类型：

- `int + int` → `int`；`byte + byte` → `int`
- `int + uint` → `long`；`long + uint` → `long`
- `int`/`long` 与 `ulong` 混合 → 编译错误（先显式统一符号域）
- `int + float` / `float + int` → `float`（对称）；任一操作数为 `double`
  → `double`

所以 `1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；赋回
更窄的类型需要显式 `as`（常量适配特例除外）。bool 与 char 不参与算术。
完整的提升与转换规则见 [类型语义](type-semantics.md)。

`string + string`（仅 OP_Add）是拼接；`"x" + 'y'`（char 隐式转 string）
与 `+` 标量操作数同样拼接。`string - string` 等是编译错误。见
[字符串](string.md)。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

比较结果一律是 **`bool`**（`true` / `false`）。

**比较操作数是有类型的**：

- **string/非 string 混合是编译错误**（`"a" < 5`、`5 == "a"`）。唯一例外是
  null 字面量：`s == null` / `c == null` 与 null 哨兵比较——对 class/引用
  操作数是同一性；对 string 操作数，null 侧读作空字符串（句柄 0 是保留的
  null 哨兵；真正的空字符串有自己的对象，按位与 null 不同）。因此
  `"" == null` 比较相等，任何非空字符串比较不等。
- **数值对**得到与算术相同的对称提升：`-2 < -1.5` 提升为 double 且为
  真；`1 == 1.0` 为真。见 [基本类型](primitives.md)。
- **char 只与 char 比较**（`'a' < 'b'` 合法）；char 与数值比较不经隐式
  转换，先 `c as int` 取码点（`a char value can only be compared with
  a char value`）。
- **字符串相等**按内容比较；字符串关系序用 C `strcmp` 式的逐字节比较（例如
  `"Z" < "a"` 为真，因为 `'Z'`(90) < `'a'`(97)）。因为字符串是 Unicode转换格式（UTF-8，Unicode Transformation Format） 且
  UTF-8 字节序等于码点序，对非 美国信息交换标准代码（ASCII，American Standard Code for Information Interchange） 文本排序也正确：`"é" > "z"` 为真。见
  [字符串](string.md)「比较」。
- **class/引用相等**（`==`、`!=`）是同一性（同一堆对象）。见 [类](class.md)。
- **带 null 操作数的算术/拼接**是编译错误——null 只有通过上面的比较同一性
  路径才有值。

### 逻辑

```nlang
a && b   a || b   !a
```

短路，C 风格：`&&` 仅在 `a` 为 `true` 时求值 `b`；`||` 仅在 `a` 为 `false`
时求值 `b`。被跳过的操作数完全没有可观察效果——没有调用、没有抛出。结果
永远是 **`bool`**（绝不是原始操作数值）。`&&`、`||`、`!` 的操作数必须是
`bool`（`operator '&&' requires bool operands`）——比较与谓词已经产生
bool。见 [语句](statements.md)「条件类型」。
