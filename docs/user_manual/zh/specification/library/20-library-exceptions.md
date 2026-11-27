> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 20. 库异常

本章把库操作的失败——第11、12与17章的内建类与方法，以及第13、14、15、16、18、19、21、22、23、24、25、26与27章的各包——映射到§8.3的内建异常类。§20.1是映射表；§20.2固定它所依据的惯例。每个位置的语义都在定义该操作的那一章里；本章不添加自己的失败行为。

## 20.1 按单元的抛出映射

| 单元 | 类 | 位置 |
| --- | --- | --- |
| `string`（§17.2） | `IndexOutOfBoundsException` | `substring`的范围错误；`charAt`越界 |
| `string`（§17.2） | `Exception` | 空实参的`split`或`replace`；对格式不当的字符串调用`toInt`、`toLong`、`toFloat`、`toDouble`、`toBool`或`toChar`；`charAt`落在延续字节上；`charCount`落在无效的Unicode转换格式（UTF-8）序列上 |
| `List`（§12.1） | `IndexOutOfBoundsException` | `get`、`set`或`removeAt`越界 |
| `Dict`（§12.2） | `Exception` | `get`没有该键的条目 |
| `math`（§13.4） | `Exception` | 对最负的`int`调用`absi`；`lo > hi`时的`clampi`或`clampf`；`min > max`时的`randomi`；对NaN或落在`long`值域之外的值调用`floor`、`ceil`或`round`；负`count`、越过16 MiB请求上限的`count`或系统随机源不可用的`randomBytes` |
| `io`（§14.3，§14.4） | `IOException` | 没有输入通道的`readLine`；输入结束时或没有输入通道（会话没有stdin）的`readToken`或`readChar`；输入结束时或没有输入通道的带类型读取器；`readFile`、`writeFile`或`appendFile`失败 |
| `io`（§14.3） | `Exception` | `readChar`落在不是有效Unicode转换格式（UTF-8）的字节序列上；带类型读取器落在格式不当的记号上（即该字符串方法的失败，§17.2） |
| 流（§14.6） | `NullPointerException` | 经由null流句柄的方法调用 |
| `system`（§15.2） | `DateTimeException` | `DateTime.parse`的畸形文本或越界日历字段 |
| `fs`（§16.3） | `IOException` | `size`、`modifiedTimeMs`、`listFiles`、`makeDirs`或`remove`失败 |
| `fs`（§16.4） | `NullPointerException` | 对`readBytes`、`writeBytes`或`readFloats`传null路径（`writeBytes`还有null`data`） |
| `fs`（§16.4） | `IOException` | `readBytes`、`writeBytes`或`readFloats`的打开、读取或写入失败（含超过16 MiB上限的文件） |
| `fs`（§16.4） | `Exception` | `readFloats`遇到非4倍数的字节数 |
| `concurrent`（§10.4） | `ChannelClosedException` | 已关闭通道上的发送或接收 |
| `concurrent`（§10.4） | `Exception` | 负容量的构造 |
| `tensor`（§19.6） | `NullPointerException` | null数组实参 |
| `tensor`（§19.6） | `IndexOutOfBoundsException` | 越过§4.10上限的创建长度；负的`reshape`维度 |
| `tensor`（§19.6） | `DivByZeroException` | 整数`div`除零 |
| `tensor`（§19.6） | `Exception` | 步长为零或无法推进的`arange`，或越过上限的范围；长度或形状不匹配；对空数组调用`max`、`min`或`mean` |
| `net`（§21.2–§21.4） | `TimeoutException` | `accept`、`connect`、`read`、`readLine`、`readExactly`、`peekFilled`、`recv`、`recvFrom`或`ping`在截止时刻结束等待 |
| `net`（§21.2–§21.4） | `SocketException` | bind、connect或选项设置器的平台失败；无法解析的主机；已关闭的套接字——含`close()`之后的`peekFilled`或`consume`，即使窗口里还有缓冲字节；同一套接字方向上停泊的第二个操作；未连接的`UdpSocket`上的`peerAddress`或`peerPort`；小于`1`的`peekFilled`阈值或不在`1…未消费数`之内的`consume`个数；提前结束的流上的`readExactly` |
| `url`（§22.1–§22.4） | `UrlException` | `Url.parse`的语法错误或超长引用；非绝对基址上的`resolve`；`percentDecode`或`formDecode`的畸形转义（含`queryParams`经`formDecode`的路径） |
| `http`（§23.2–§23.8） | `HttpException` | 非绝对或scheme不是`http`的请求URL；含回车符或换行符的头名或头值；越过`maxHeaderBytes`或`maxHeaders`的头部块（431）；越过`maxBodyBytes`的正文读取（413）；从对端读到的畸形消息；重定向过多；跨重定向重放的流式正文；已关闭客户端池上的调用 |
| `http`（§23.4–§23.6） | `TimeoutException` | `HttpBodyReader.read`在截止时刻结束等待；超过`callTimeoutMs`的连接池等待（客户端调用之下的`net`失败原样透出，§23.8） |
| `http`（§23.5） | `Exception` | 实例的监听仍在运行时再次调用`start`或`serve`——消息点名所用的入口 |
| `gzip`（§24.1） | `GzipException` | 被编码器拒绝的`compress`；未能解出一条完整流的输入的`decompress` |
| `regex`（§25.2、§25.3） | `RegexException` | 被引擎拒绝的模式的`Regex`构造；下标越界或组名未知的`group` |
| `json`（§26.3–§26.7） | `JsonException` | 格式错误、顶层值之后有多余字节、或嵌套超过深度上限的`parse`（各带字节偏移）；对另一种类别的值调用访问器；`long`或`int`范围之外的`getLong`或`getInt`；缺失键的`get`；超过嵌套上限或非有限`double`的`serialize` |
| `web`（§27.4–§27.9） | `HttpException` | 链路已推进之后再调用`next()`的中间件（500）；urlencoded或multipart文法拒绝的请求正文（400）；表单正文总量、分部数或单个分部越过各自配置限额（413） |
| `web`（§27.2–§27.9） | `Exception` | 从未调用`useSessions`的应用上的`RequestContext.session()`（§27.3）；首个请求之后才尝试的`App.use`、`App.useSessions`或路由登记；本身非法或与既有行冲突的路由模式；未配`Secure`就要求的`__Host-`前缀；根不是目录的`StaticFiles`构造，以及扩展名或媒体类型为空的`addType`；id或事件名含回车符或换行符的`Sse.send`；`MultipartBody.addField`或`addFile`的实参拒绝，以及八次重掷边界全部落空的碰撞扫描 |
| `web`（§27.8） | `ChannelClosedException` | 客户端断开之后对服务器推送事件流的`send`——断开即关闭该流的通道，该类本就是通道的类（§10.4） |

经由`null`接收者的调用在每个单元里都抛出`NullPointerException`（§11.2）；表中各行只列定义章节除此之外另行点名的位置。

## 20.2 惯例

- 库声明九个自己的异常类：`ChannelClosedException`，`Exception`的直接子类，由`concurrent`包声明；`SocketException`与`TimeoutException`，`IOException`的子类，由`net`包声明；`UrlException`，`IOException`的子类，由`url`包声明；`HttpException`与`GzipException`，`IOException`的子类，分别由`http`与`gzip`包声明；`RegexException`，`IOException`的子类，由`regex`包声明；`JsonException`，`Exception`的直接子类，由`json`包声明；以及`DateTimeException`，`Exception`的直接子类，由`system`包声明（§8.3）。其余每个库失败都抛出§8.3的类（§11.4）。用户代码可以声明自己的类（§8.4）。
- 基类`Exception`是没有专属类的实参、范围与解析错误所用的类；不存在`IllegalArgumentException`或同级类。
- 抛出不是唯一的失败通道。谓词与查询以返回值报告：`fs`的谓词与缺失路径上的`remove`（§16.3）、`io.hasInput`（§14.3），以及`math`超越函数家族产出NaN的定义域错误（§13.4）。null句柄之外的流失败是*宿主级*执行期错误（§14.6，§8.1）：没有`catch`子句处理它们。
