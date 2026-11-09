> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 11. 库约定

## 11.1 范围与包接口面

标准库由`math`、`io`、`fs`、`thread`、`concurrent`、`tensor`、`net`、`system`、`url`、`http`与`gzip`十一个包（各是一组包限定的自由函数）与`string`方法（经接收者分派）组成。两个预置包与之并列：`lang`（`stdlib/lang.n`）声明异常家族、层次根`Object`、声明持有类`String`与泛型数组面`Array<T>`；`containers`（`stdlib/containers.n`）声明基础容器`List<T>`与`Dict<K,V>`。每次构建自动导入两者，其声明无需任何import语句即可见，显式写`import lang;`或`import containers;`也合法且自动去重（§3.9.4）。两者声明的恒可见名字——`Object`、`Exception`家族、`String`、`Array`、`List<T>`、`Dict<K,V>`——即全部集合（§3.9.4）。

内建值类与容器类的每个方法都是内建的：其声明位于预置包，每次构建都自动导入，程序不能替换它——同名用户类是普通类，不会接管内建接收者。预置类中的`String`只作声明持有：它声明`string`关键字的方法面，不能实例化、不能继承、也不能用作类型，且不在类层次之内——`string`关键字就是值类型的写法。预置类`Array`则走向另一边：`Array<T>`是真实的泛型类型（§4.10.1），其值经方括号创建形式构造；类本身仍只持有数组方法声明，不能被继承。

方法面分布在本规范的各处：`string`方法在第17章，`List`与`Dict`方法在第12章，每个值的默认渲染在§11.2，并发句柄类与`thread`、`concurrent`的包类在第10章、第18章，流句柄类在第14章，异常类在§8.3。

包由其文件路径标识——`stdlib/io.n`就是包`io`——同一次构建中每个包名只能存在一份：重复是编译错误，诊断指名两条来源，而名为`io`的项目目录就是普通目录。调用包函数要先`import`该包：`import math;`之后限定名`math.sin(x)`与裸名`sin(x)`都可调用（§3.9.4）。包名用作值（`int x = math;`）无法解析——包不是值。分工上，`io`承担内容输入输出（IO），`math`承担数值计算，`fs`承担文件系统的命名与元数据，`tensor`承担数值数组内核（第19章），`net`承担网络套接字（第21章），`url`承担URL编解码（第22章），`http`承担HTTP/1.1的服务器与客户端（第23章），`gzip`承担整块字节的gzip压缩（第24章），`system`承担进程与时刻（第15章），`thread`承担虚拟线程的派生与挂起原语，`concurrent`承担配合类（§3.9.4）。

签名——参数种类、元数与返回类型——写在随工具链分发的`stdlib/*.n`声明中，经语言服务的符号索引提供给编译器与编辑器（补全、悬停、转到定义）（§3.9.4）。标准库的形状与第三方库一致：`.n`源码声明接口面，其中的`native`成员由`nlang_<ns>.dll`实现，运行期经宿主应用二进制接口（ABI）到达。`thread`与`concurrent`两包是例外：它们的`native`声明由虚拟机本体实现，经内建通道分发。限定调用按声明做类型检查后走普通函数调用路径；内建string与数组成员方法按预置声明做类型检查，经接收者直接进入内建实现。

## 11.2 默认渲染

没有声明`toString`的类按§4.11.2所述渲染：类的源码名、`@`，以及一个由实现选定的对象编号的小写十六进制渲染（`Point@1a`）。`null`接收者抛出`NullPointerException`（§4.8.10、§8.3）。

每个内建类型按如下渲染；`List`、`Dict`、类与函数值也作为方法响应`toString()`：

| 接收者 | 渲染 |
| --- | --- |
| `string` | 字符串本身 |
| `List` | `[e1, e2, ...]`——元素按下文规则渲染 |
| `Dict` | `{k1: v1, k2: v2, ...}`——条目按下文规则渲染，按插入序（§12.2） |
| 数组 | `[...]`，按维度嵌套（§4.10、§5.3） |
| 枚举 | 成员名（§4.6） |
| 函数值 | `func name`或`method name`（§7.4） |
| 装有装箱值的`Object` | 该值按其类型渲染（§4.11） |

在容器渲染内部：

- `string`元素带引号出现，其中`\`、`"`、换行、回车与制表符分别转义为`\\`、`\"`、`\n`、`\r`与`\t`；
- `null`元素出现为`<null>`；
- `struct`元素出现为`<struct>`（结构体不带`toString`，§4.7）；
- 嵌套的容器、数组、装箱值或函数值按其自身的规则渲染。

渲染至多嵌套64层；嵌套更深的结构——经由`Object`字段的环——在`toString`处抛出宿主级执行期错误（§8.1）。

## 11.3 string方法

`string`方法面在第17章定义。

## 11.4 库约定

标准库遵循同一套接口约定。

- 类型用PascalCase；方法、函数与变量用camelCase；枚举成员用PascalCase；类型参数是单个大写字母；`main`小写（§3.2）。
- 纯访问器可以带`get`前缀（`getHashCode`）；主流风格是裸名（`length`、`count`、`keys`）。内建类的方法对谓词用裸名（`contains`、`startsWith`、`containsKey`）；包函数可以带`is`或`has`前缀（`fs.isFile`、`io.hasInput`）。
- 失败经由抛出§8.3的内建异常类来报告，外加七个包声明的类——`concurrent`的`ChannelClosedException`、`net`的`SocketException`与`TimeoutException`、`url`的`UrlException`、`http`的`HttpException`、`gzip`的`GzipException`、`system`的`DateTimeException`（§8.3）。第20章把每个库操作映射到它所抛出的类。
- 文本输出经由`toString`渲染值（§5.3、§11.2）；控制台函数的实参面在第14章。
- 库调用的实参按声明的参数类型与转换规则检查——同类型直接接受，规则允许的隐式加宽自动施加（整型家族与`float`实参进入`double`形参，如`math.sqrt(4)`可编译），收窄一律显式`as`（§4.4、§7.2）。§4.5的基本类型到字符串的强制转换在包调用处不触发：库的`string`形参只接受`string`。io的三个做强制转换的函数（`write`、`print`、`eprint`）是唯一例外（§14.2）。
