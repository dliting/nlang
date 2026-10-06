# 标准库


NLang的标准库由`math`、`io`、`fs`三个包（包限定的自由函数）与string方法（经接收者分派）组成。包由其文件路径标识——`stdlib/io.n`就是包`io`——同一次构建中每个包名只能存在一份（重复是编译错误，诊断指名两条来源；名为`io`的项目目录就是普通目录）。调用只写限定名（`math.sin(x)`）；裸名不在作用域内。包名用作值（`int x = math;`）无法解析——包不是值。分工上，io承担内容输入输出（IO，input/output），math承担数值计算，fs承担文件系统的命名空间与元数据。

标准库的**签名**（参数类型、参数个数、返回类型）写在随工具链分发的`stdlib/*.n`声明中，经语言服务的符号索引提供给编译器与编辑器（代码补全、悬停、转到定义）。标准库的形状与第三方库一致：`stdlib/*.n`声明接口，其中的`native`成员由`nlang_<ns>.dll`实现，运行期经宿主应用二进制接口（ABI，application binary interface）到达。限定调用按声明做类型检查后发射`OP_CallFunc`；内建string方法经接收者分派，发射`OP_CallIntrinsic`。查找`.n`与加载native库的目录规则见[库与搜索路径](stdlib-search-paths.md)。

**参数类型**：与声明的kind按转换矩阵逐实参审查——同kind原样放行，矩阵允许的隐式加宽自动施加（整型家族与float实参进入`double`形参，如`math.sqrt(4)`；收窄一律显式`as`——`math.absi(1.5)`是编译错误）。唯一的例外是io的强制转换三函数（`write`/`print`/`eprint`），它们接受string、数组、全部标量基本类型与函数值（调用点转换；函数值格式化为`func <name>`，见[函数类型与代理](function-types-and-delegates.md)）；class与enum值打印前需要显式`.toString()`（struct实参直接拒绝——struct没有`toString`）。

详细参考按主题分页：

- [math——25个函数](stdlib-math.md)
- [io——内容IO](stdlib-io.md)（含取词读取与混合语义）
- [fs——名字、目录、元数据](stdlib-fs.md)
- [string方法——18个内建](stdlib-string.md)
- [流——ByteStream与FileStream](stdlib-streams.md)
- [库与搜索路径](stdlib-search-paths.md)（含nide配置与第三方库形状）
- [异常映射](stdlib-exceptions.md)
