# 标准库


NLang的标准库由`math`、`io`、`fs`、`thread`、`concurrent`、`tensor`六个包（包限定的自由函数）与string方法（经接收者分派）组成。**预置包**`builtins`（`stdlib/builtins.n`）与之并列：每次构建自动导入，其声明无需任何import语句即可见（显式写`import builtins;`也合法，会自动去重）。包内以普通源码形式声明编译器的非泛型内建类——句柄类`Mutex`、`Semaphore`、`Thread`，异常家（`Exception`及其七个内建子类），以及层次根`Object`。这些签名（构造器元数、方法面）就是文件里声明的样子，语言服务对它们与其他声明一视同仁。泛型容器`List<T>`/`Dict<T>`/`Channel<T>`/`Future<T>`与流类`ByteStream`/`FileStream`不在其中——它们仍是编译器内建（见[内建泛型类](builtin-generic-classes.md)与[流](stdlib-streams.md)）。包由其文件路径标识——`stdlib/io.n`就是包`io`——同一次构建中每个包名只能存在一份（重复是编译错误，诊断指名两条来源；名为`io`的项目目录就是普通目录）。调用只写限定名（`math.sin(x)`）；裸名不在作用域内。包名用作值（`int x = math;`）无法解析——包不是值。分工上，io承担内容输入输出（IO，input/output），math承担数值计算，fs承担文件系统的命名空间与元数据，tensor承担数值数组的创建、逐元素与归约内核（见[tensor](stdlib-tensor.md)），thread承担虚拟线程的派生与挂起原语，concurrent承担同步工具`WaitGroup`（句柄类不属于任何函数包——见上文预置包`builtins`与[并发](concurrency.md)）。

标准库的**签名**（参数类型、参数个数、返回类型）写在随工具链分发的`stdlib/*.n`声明中，经语言服务的符号索引提供给编译器与编辑器（代码补全、悬停、转到定义）。标准库的形状与第三方库一致：`stdlib/*.n`声明接口，其中的`native`成员由`nlang_<ns>.dll`实现，运行期经宿主应用二进制接口（ABI，application binary interface）到达（`thread`包是唯一例外：其`native`声明由虚拟机本体实现，经内建通道分发）。限定调用按声明做类型检查后按普通函数调用处理；内建string方法经接收者直接进入内建实现。查找`.n`与加载native库的目录规则见[库与搜索路径](stdlib-search-paths.md)。

**参数类型**：逐实参按声明的参数类型与转换规则检查——同类型直接接受，转换规则允许的隐式加宽自动施加（整型家族与float实参进入`double`形参，如`math.sqrt(4)`；收窄一律显式`as`——`math.absi(1.5)`是编译错误）。唯一的例外是io的三个做强制转换的函数（`write`/`print`/`eprint`），它们接受string、数组、全部标量基本类型与函数值（调用点转换；函数值格式化为`func <name>`，见[函数类型与委托](function-types-and-delegates.md)）；class对象与enum变量打印前需要显式`.toString()`（枚举成员常量可直接打印成员名；struct实参直接拒绝——struct没有`toString`）。

详细参考按主题分页：

- [math——25个函数](stdlib-math.md)
- [tensor——数值数组内核](stdlib-tensor.md)
- [io——内容IO](stdlib-io.md)（含取词读取与混合语义）
- [fs——名字、目录、元数据](stdlib-fs.md)
- [并发——虚拟线程、Mutex、Channel、Future与同步工具](concurrency.md)
- [string方法——18个内建](stdlib-string.md)
- [流——ByteStream与FileStream](stdlib-streams.md)
- [库与搜索路径](stdlib-search-paths.md)（含nide配置与第三方库形状）
- [异常映射](stdlib-exceptions.md)
