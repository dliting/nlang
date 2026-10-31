> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 13. 内建类与库约定

## 13.1 范围

预置名字恒在作用域，无需导入（§12.4）：`Object`、§9.3的`Exception`家族、`String`、`Array`、`List<T>`、`Dict<K,V>`——即全部集合。本章定义内建值类与容器类的方法面——`Object`协议（§13.2）、`string`方法（§13.3）与`List`和`Dict`方法（§13.4）——以及标准库遵循的约定（§13.5）。

在其他处定义的方法面：并发句柄类（`Mutex`、`Semaphore`、`AtomicInt`、`Channel`、`Thread`、`Future`）——`concurrent`与`thread`的包类（第11章、第17章），流句柄类（`ByteStream`、`FileStream`）在第15章，异常类在§9.3。

本章每个方法都是内建的：其声明位于预置包`builtins`（§12.4）——每次构建都自动导入——程序不能替换它；同名用户类是普通类，不会接管内建接收者。预置类中的`String`与`Array`只作声明持有：它们声明`string`关键字与数组类型的方法面，不能实例化、不能继承、也不能用作类型，且不在类层次之内——`string`与方括号数组语法才是值类型的写法。

## 13.2 Object协议

每个类都从`Object`继承三个协议方法（§4.9）：`int equals(Object other);`、`int getHashCode();`与`string toString();`。它们的虚属性与重写由第8章定义（§8.2）。默认实现比较对象身份：接收者与实参指代同一对象时`equals`产出`1`，否则产出`0`；`getHashCode`对相等的对象产出相等的`int`。两者都不可经由`null`接收者到达：经由`null`的成员访问抛出`NullPointerException`（§9.3）。

`string`把这一对重写为按内容：当且仅当两侧持有相同字节时`equals`产出`1`——与`==`相同的关系（§4.4）——而`getHashCode`对字节做哈希，因此相等的字符串哈希相等。

### 默认渲染

`toString`把值渲染为文本。没有声明`toString`的类渲染为类的源码名、`@`，以及一个由实现选定的对象编号的小写十六进制渲染：`Widget@7f3a`。`null`接收者抛出`NullPointerException`（§9.3）。

每个内建类型按如下渲染；`List`、`Dict`、类与函数值也作为方法响应`toString()`：

| 接收者 | 渲染 |
| --- | --- |
| `string` | 字符串本身 |
| `List` | `[e1, e2, ...]`——元素按下文规则渲染 |
| `Dict` | `{k1: v1, k2: v2, ...}`——条目按下文规则渲染，按插入序（§6.4） |
| 数组 | `[...]`，按维度嵌套（§4.3、§5.3） |
| 枚举 | 成员名（§4.5） |
| 函数值 | `func name`或`method name`（§4.11） |
| 装有装箱值的`Object` | 该值按其类型渲染（§4.9） |

在容器渲染内部：

- `string`元素带引号出现，其中`\`、`"`、换行、回车与制表符分别转义为`\\`、`\"`、`\n`、`\r`与`\t`；
- `null`元素出现为`<null>`；
- `struct`元素出现为`<struct>`（结构体不带`toString`，§4.6）；
- 嵌套的容器、数组、装箱值或函数值按其自身的规则渲染。

渲染至多嵌套64层；嵌套更深的结构——经由`Object`字段的环——在`toString`处抛出宿主级执行期错误（§9.1）。

## 13.3 string方法

`string`方法在字节上操作（§4.4）：偏移是字节偏移，大小写转换只触及美国信息交换标准代码（ASCII）字母。码点层——`charAt`与`charCount`——在字节核之上解码Unicode转换格式（UTF-8）。

| 方法 | 签名 | 失败行为 |
| --- | --- | --- |
| `substring(start[, end])` | → `string` | 字节`[start, end)`；`end`默认为`length()`；`start`或`end`落在`[0, length()]`之外，或`end < start`，抛出`IndexOutOfBoundsException` |
| `indexOf(t)` | → `int` | `t`的首个字节偏移，不存在则为`-1`；空`t`产出`0` |
| `startsWith(t)` / `endsWith(t)` / `contains(t)` | → `bool` | — |
| `toUpper()` / `toLower()` | → `string` | 仅ASCII字母 |
| `trim()` | → `string` | 剥去首尾的空格、制表符、换行、回车、换页与垂直制表 |
| `split(sep)` | → `List<string>` | 空`sep`抛出`Exception` |
| `replace(old, repl)` | → `string` | 每一处不重叠的出现，从左到右；空`old`抛出`Exception` |
| `toInt()` / `toLong()` | → `int` / `long` | 对整个字符串做严格解析；格式不当的字符串抛出`Exception` |
| `toFloat()` / `toDouble()` | → `float` / `double` | 对整个字符串做严格解析；格式不当的字符串抛出`Exception` |
| `toBool()` | → `bool` | 恰为`"true"`或`"false"`；其他任何值抛出`Exception` |
| `toChar()` | → `char` | 十进制码点数；落在Unicode标量值之外的值抛出`Exception` |
| `charAt(byteIndex)` | → `char` | 从该字节偏移开始的码点；越界抛出`IndexOutOfBoundsException`，延续字节抛出`Exception` |
| `charCount()` | → `int` | 码点数；无效字节序列抛出`Exception` |

`string`接收者也响应`length()`——字节数（§4.4）——以及`equals`、`getHashCode`与`toString`（§13.2）。`equals`取`string`实参；凡形参取`string`的位置都接受null实参，按空字符串读出。下标`s[i]`把字节`i`以`ubyte`读出（§5.7）。

## 13.4 List与Dict方法

`List<T>`（§4.10）是一个可增长的序列：

| 方法 | 效果 | 失败行为 |
| --- | --- | --- |
| `add(value)` | 追加`value` | — |
| `get(i)` | 位置`i`处的元素 | 越界抛出`IndexOutOfBoundsException` |
| `set(i, value)` | 替换`i`处的元素 | 越界抛出`IndexOutOfBoundsException` |
| `length()` | 元素数 | — |
| `removeAt(i)` | 移除`i`处的元素，后续元素下移 | 越界抛出`IndexOutOfBoundsException` |
| `indexOf(value)` | 匹配`value`的首个位置，没有则为`-1` | — |
| `contains(value)` | 是否有某个位置匹配`value` | — |
| `clear()` | 移除所有元素 | — |
| `toString()` | `[e1, e2, ...]`（§13.2） | — |

`indexOf`与`contains`按§4.10的区分种类的相等性匹配：基本类型元素按值，`string`元素按内容，其他引用元素按身份。

`Dict<K, V>`（§4.10）把键映射到值：

| 方法 | 效果 | 失败行为 |
| --- | --- | --- |
| `set(key, value)` | 把`value`存到`key`下；重新存一个键会替换值并保持该键的位置 | — |
| `get(key)` | `key`下所存的值 | `key`没有条目抛出`Exception` |
| `containsKey(key)` | `key`的条目是否存在 | — |
| `remove(key)` | 移除`key`的条目；产出是否移除了一个 | — |
| `clear()` | 移除所有条目 | — |
| `count()` | 条目数 | — |
| `keys()` | 一个新的`List<K>`，含按插入序的各键（§6.4） | — |
| `toString()` | `{k1: v1, k2: v2, ...}`，按插入序（§13.2） | — |

键匹配就是§4.10的区分种类的相等性。下标语法糖`li[i]`与`d[k]`映射到`get`与`set`（§5.7）；容器的存入或取出按§10.1的类别移动值。

## 13.5 库约定

标准库遵循同一套接口约定。

- 类型用PascalCase；方法、函数与变量用camelCase；枚举成员用PascalCase；类型参数是单个大写字母；`main`小写（§3.2）。
- 纯访问器可以带`get`前缀（`getHashCode`）；主流风格是裸名（`length`、`count`、`keys`）。内建类的方法对谓词用裸名（`contains`、`startsWith`、`containsKey`）；包函数可以带`is`或`has`前缀（`fs.isFile`、`io.hasInput`）。
- 失败经由抛出§9.3的内建异常类来报告，外加唯一一个包声明的类——`concurrent`的`ChannelClosedException`（§9.3）。第19章把每个库操作映射到它所抛出的类。
- 文本输出经由`toString`渲染值（§5.3、§13.2）；控制台函数的实参面在第15章。
