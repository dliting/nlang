> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 17. string方法

`string`方法是分派到`string`接收者上的内建操作：搜索、切片、大小写转换、解析与码点访问。§17.1固定接收者及其字节语义；§17.2定义十九个方法。

## 17.1 接收者与字节语义

对`string`接收者的方法调用——`s.substring(1)`——作用于任何`string`表达式，字面量也不例外（`"abc".toUpper()`）。每个字符串方法都由预置类`String`声明（§11.1）：该类只为持有声明而存在，不能实例化、不能继承、也不能用作类型，且值类型以`string`关键字书写（§4.3）。

预置类`String`另有一个常量成员：`string.EMPTY`是空字符串`""`的具名形式。

各方法在字节上操作（§4.3）：偏移是字节偏移，大小写转换只触及美国信息交换标准代码（ASCII）字母。Unicode转换格式（UTF-8）的字节序与码点序一致，所以§4.3的关系比较在UTF-8内容上有确定的结果。码点层——`charAt`与`charCount`——在字节核之上解码UTF-8：码点访问（`charAt`、`charCount`、对字符串的`foreach`）建立在字节访问之上，正如§4.3「char桥接」所述。

## 17.2 方法

`string`接收者响应下列十九个方法。

| 方法 | 签名 | 失败行为 |
| --- | --- | --- |
| `substring(start[, end])` | → `string` | 字节`[start, end)`；`end`默认为`length()`；`start`或`end`落在`[0, length()]`之外，或`end < start`，抛出`IndexOutOfBoundsException` |
| `indexOf(t[, from])` | → `int` | 自`from`起`t`的首个字节偏移，不存在则为`-1`；`from`默认为`0`，负`from`按`0`计，越过末尾的`from`产出`-1`（空`t`亦然）；其余情况下空`t`在`max(from, 0)`处命中 |
| `equalsIgnoreCase(t)` | → `bool` | 仅ASCII大小写折叠：只有`A-Z`折叠为`a-z`，其他字节必须逐字节相等 |
| `startsWith(t)` / `endsWith(t)` / `contains(t)` | → `bool` | — |
| `toUpper()` / `toLower()` | → `string` | 仅ASCII字母 |
| `trim()` | → `string` | 剥去首尾的空格、制表符、换行、回车、换页与垂直制表 |
| `split(sep)` | → `List<string>` | 空`sep`抛出`Exception` |
| `replace(old, repl)` | → `string` | 每一处不重叠的出现，从左到右；空`old`抛出`Exception` |
| `toInt([radix])` / `toLong()` | → `int` / `long` | 对整个字符串做严格解析；`radix`默认为`10`且须在`2..36`内，不接受`0x`前缀；格式不当的字符串、越界的`radix`或溢出抛出`Exception` |
| `toFloat()` / `toDouble()` | → `float` / `double` | 对整个字符串做严格解析；格式不当的字符串抛出`Exception` |
| `toBool()` | → `bool` | 恰为`"true"`或`"false"`；其他任何值抛出`Exception` |
| `toChar()` | → `char` | 十进制码点数；落在Unicode标量值之外的值抛出`Exception` |
| `charAt(byteIndex)` | → `char` | 从该字节偏移开始的码点；越界抛出`IndexOutOfBoundsException`，延续字节抛出`Exception` |
| `charCount()` | → `int` | 码点数；无效字节序列抛出`Exception` |

`string`接收者也响应`length()`——字节数（§4.3）——以及协议方法`equals`、`getHashCode`与`toString`（§4.11.2、§11.2）：`equals`比较内容，与`==`同一关系，取`string`实参；`getHashCode`按内容哈希，相等的字符串哈希相等。形参取`string`的位置不接受null实参——null不是字符串值（§4.3）。下标`s[i]`把字节`i`以`ubyte`读出（§5.7）。
