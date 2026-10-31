> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 19. 库异常

本章把库操作的失败——第13章的内建类与第14–18章的各包——映射到§9.3的内建异常类。§19.1是映射表；§19.2固定它所依据的惯例。每个位点的语义都在定义该操作的那一章里；本章不添加自己的失败行为。

## 19.1 按单元的抛出映射

| 单元 | 类 | 位点 |
| --- | --- | --- |
| `string`（§13.3） | `IndexOutOfBoundsException` | `substring`的范围错误；`charAt`越界 |
| `string`（§13.3） | `Exception` | 空实参的`split`或`replace`；对格式不当的字符串调用`toInt`、`toLong`、`toFloat`、`toDouble`、`toBool`或`toChar`；`charAt`落在延续字节上；`charCount`落在无效的Unicode转换格式（UTF-8）序列上 |
| `List`（§13.4） | `IndexOutOfBoundsException` | `get`、`set`或`removeAt`越界 |
| `Dict`（§13.4） | `Exception` | `get`没有该键的条目 |
| `math`（§14.4） | `Exception` | 对最负的`int`调用`absi`；`lo > hi`时的`clampi`或`clampf`；`min > max`时的`randomi`；对NaN或落在`long`值域之外的值调用`floor`、`ceil`或`round` |
| `io`（§15.3，§15.4） | `IOException` | 没有输入通道的`readLine`；输入结束时或没有通道的`readToken`或`readChar`；输入结束时的带类型读取器；`readFile`、`writeFile`或`appendFile`失败 |
| `io`（§15.3） | `Exception` | `readChar`落在不是有效Unicode转换格式（UTF-8）的字节序列上；带类型读取器落在格式不当的记号上（即该字符串方法的失败，§13.3） |
| 流（§15.5） | `NullPointerException` | 经由null流句柄的方法调用 |
| `fs`（§16.3） | `IOException` | `size`、`listFiles`、`makeDirs`或`remove`失败 |
| `concurrent`（§11.4） | `ChannelClosedException` | 已关闭通道上的发送或接收 |
| `concurrent`（§11.4） | `Exception` | 负容量的构造 |
| `tensor`（§18.5） | `NullPointerException` | null数组实参 |
| `tensor`（§18.5） | `IndexOutOfBoundsException` | 越过§4.3上限的创建长度；负的`reshape`维度 |
| `tensor`（§18.5） | `DivByZeroException` | 整数`div`除零 |
| `tensor`（§18.5） | `Exception` | 步长为零或无法推进的`arange`，或越过上限的范围；长度或形状不匹配；对空数组调用`max`、`min`或`mean` |

经由`null`接收者的调用在每个单元里都抛出`NullPointerException`（§13.2）；表中各行只列定义章节除此之外另行点名的位点。

## 19.2 惯例

- 库声明一个自己的异常类：`ChannelClosedException`，`Exception`的直接子类，由`concurrent`包声明（§9.3）。其余每个库失败都抛出§9.3的类（§13.5）。用户代码可以声明自己的类（§9.4）。
- 基类`Exception`是没有专属类的实参、范围与解析错误所用的类；不存在`IllegalArgumentException`或同级类。
- 抛出不是唯一的失败通道。谓词与查询以返回值报告：`fs`的谓词与缺失路径上的`remove`（§16.3）、`io.hasInput`（§15.3），以及`math`超越函数家族产出NaN的定义域错误（§14.4）。null句柄之外的流失败是*宿主级*执行期错误（§15.5，§9.1）：没有`catch`子句处理它们。
