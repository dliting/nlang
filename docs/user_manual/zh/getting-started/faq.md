# 常见问题

### 编译产物写到哪里了？

`ncc build hello.n` 不带 `-o` 时，`.ncu` 写在**当前工作目录**，
不是源文件旁边；`-p` 项目形态的 `.npkg` 写在 **.nproj 所在目录**
——在别的目录里找不到产物时先想到这一点。各形态的缺省规则与
优先级见[ncc](../cli-tools/ncc.md)。

nide 侧由输出位置优先级决定（详见
[三种运行方式](running.md#配置-nide)）：

- 工程：`.nproj` 的 `outputDir` > nide 全局构建输出目录 > 工程目录；
- 独立 `.n` 文件：全局构建输出目录 > 缺省位置（用户临时目录下的
  `nlang-nide` 子目录）。

### 运行时报 `module '...' not found`？

产物不含库代码：`.ncu`/`.npkg` 只携带自己的单元，外部模块在**运行期**
沿库搜索路径装载链接。报错形如
`Runtime error: nloader failed:`（正文行
`module 'lib' not found (searched: ...)`），括号里列出了
已搜索的全部目录——把依赖所在目录加进 `-I` 或 `NLANG_PATH`，或把
依赖放在产物旁边。标准库随工具链分发（`stdlib\`），无需手动配置。
机制详见 → [ncc](../cli-tools/ncc.md) 的「产物与加载期链接」。

### 退出码不是想要的值？

进程退出码就是 `main` 的返回值，Windows 保留 32 位原值；但 POSIX shell
（bash、Git-Bash、持续集成（CI，continuous integration） 的 bash 步骤）按惯例只保留低 8 位——`return 300`
在 Python/cmd/PowerShell 里看到 300，在 bash 里看到 44（对 256 取模）。
测试约定预期值 0–255，让所有观察者的视图一致。

详见 → [语言规格/退出码约定](../language-spec/exit-code-convention.md)。

### 中文输出乱码？

程序输出是 Unicode转换格式（UTF-8，Unicode Transformation Format） 字节。工具链把进程活动代码页设为 UTF-8（工具内嵌
清单声明，Windows 10 1903+ 生效），并在启动时把所在控制台切换到
UTF-8 代码页，默认控制台即可正常显示中文输出，无需手动 `chcp 65001`。
`fs` 与 `io` 的非 美国信息交换标准代码（ASCII，American Standard Code for Information Interchange） 路径、文件名同样按 UTF-8 往返。重定向到文件
的输出字节不受影响——用非 UTF-8 编码打开它的编辑器仍会显示乱码。
该代码页切换在工具退出后仍对同一控制台窗口生效，之后其中按旧编码
输出的程序可能显示为乱码。

详见 → [语言规格/标准库](../language-spec/standard-library.md)。

### 编译报 `not valid UTF-8`？

源文件与 `.nproj` 项目文件必须保存为 UTF-8：ncc 在词法前对
整个文件做严格校验，无效字节被具名拒绝（`Source file is not
valid UTF-8 ... (first invalid byte at line N). Save the file as
UTF-8.`），UTF-16 保存的文件得到专门提示（改用 UTF-8 重新保存
即可）。这能拦住旧编码字节静默混入字符串常量的隐含错误。nide
的构建经由 ncc，同样受此门控。文件
开头的 UTF-8 字节顺序标记（BOM，byte order mark） 被接受并跳过，编辑器的「UTF-8 with BOM」保存
形式无需处理。详见 →
[语言规格/基本类型](../language-spec/primitives.md)。

### 帮助文档与搜索在哪？

nide 帮助菜单的「NLang 入门」「语言规格」「VM 架构」「命令行工具」
都在 集成开发环境（IDE，integrated development environment） 内嵌的帮助窗口中打开（内容即安装目录 `docs\site\` 下的
文档站），左侧是导航目录。搜索框在窗口左上角（站点标题旁），支持
全文检索。

### `v + 1` 不加空格报 `syntax error`？

词法层把二元 `+` 后紧跟的数字吃成带符号字面量——`v+1` 被读成标识符
`v` 后接字面量 `+1`，两个表达式连排不构成合法语句，于是报
`syntax error`（`Invalid statement.`）。规避：加号两侧留空格
（`v + 1`）。

详见 → [语言规格/表达式](../language-spec/expressions.md)。

### 调试时怎么给 `io.readLine` 输入 / 停止后 `finally` 不执行 / 断点错位？

运行与调试的标准输入都在「运行输出」页底部的输入行：会话运行期间
输入一行并回车，该行就送达程序的下一次读取（详见
[在 nide 中调试](debugging.md)）；停止调试是硬终止，进程直接结束
（`finally` 不执行）；会话内不跟踪行号漂移，一次会话对应一份行号
快照，会话中编辑或重新构建不受支持，须重新打开调试会话。详见
[在 nide 中调试](debugging.md) 的「v1 已知限制」一节。

### 跨模块引用报 `Module '...' is not imported`？

`import` 只开放**限定名**——`import lib;` 之后只能写 `lib.f()`，
裸名 `f()` 不解析。各引用形式的可见性规则见
[语言规格/声明](../language-spec/declarations.md) 的「import 声明」。
完整错误消息见 → [常见错误消息](../language-spec/common-errors.md)。

### 数组值赋给其他类型报 `Incompatible type`？

数组值只有两类合法去向——自身数组类型与全部 `string` 目标
（`toString` 等），其余标量上下文一律在编译期具名拒绝
（`Incompatible type "a"`）。完整规则见
[语言规格/已知限制](../language-spec/known-limitations.md) 的
「标量上下文中的数组值」条。完整错误消息见 →
[常见错误消息](../language-spec/common-errors.md)。

### `.ncu` 版本过时，提示重新编译？

`.ncu` 格式地板只升不降：旧 ncc 产出的模块会被加载器拒为过时
（`Module version ... is outdated; recompile with current ncc`），
须用当前工具链重新编译。各版本地板与语义变更见
[VM 架构/模块序列化](../vm-architecture/module-serialization.md)
的「版本历史」，每次抬升的缘由在 CHANGELOG 对应版本节。

### 跨模块函数值 / 复杂默认参数 / 具名实参被拒？

`.ncu` 的类型描述符只承载数据类型，不携带 `Func` 签名与形参名，
因此以下跨模块形态被消费侧编译期拒绝：引用被导入函数作为函数值、
给被导入函数传函数引用、非常量折叠的默认参数、以及被导入函数的具名
实参（请只用位置实参）。详见
[语言规格/已知限制](../language-spec/known-limitations.md) 的「跨模块
函数值被拒绝而非搬运」「被导入函数的默认参数」「被导入函数的具名
实参」条。完整错误消息见 →
[常见错误消息](../language-spec/common-errors.md)。

### 条件 / `&&` / `||` / `!` 报「必须 bool」？参数超 64？

`if` / `while` / `do-while` / `for` / `assert` 的条件与 `&&` / `||` /
`!` 的操作数都必须是 `bool`（比较与谓词已经产生 bool——没有 C 式的
「非零即真」）；int、string、float、char、class、
struct、array 都是编译期具名拒绝（`if condition must be bool, not
"Int32"`、`operator '&&' requires bool operands, got "Int32"`）。计数
判断请写 `if (count != 0)`。另
外函数参数数有合理性上限 64，超出触发编译期错误
（`function "f" has 65 parameters; limit is 64.`）。条件类型的机制
见 [语言规格/语句](../language-spec/statements.md) 的「条件类型」，
参数数上限见 [语言规格/已知限制](../language-spec/known-limitations.md)
的「参数数上限」条。完整错误消息见 →
[常见错误消息](../language-spec/common-errors.md)。

### 成员链 `s.length().toString()` 让 ncc 崩溃？

这是**已知的编译器缺陷**（当前 ncc 仍复现 `ncc: internal crash
(code 0xC0000005)`）——对 `string` 等成员链做 `s.length().toString()`
这类链式成员调用（在方法调用结果上再链一个成员访问）会在编译期段错误，
而非给出具名诊断。规避：用中间局部变量承接
（`int n = s.length(); string t = n.toString();`）。

### 帮助窗口提示「文档未找到」？

帮助窗口按当前语言树在 `docs\site\` 邻接目录定位对应的 `.html`；
找不到时提示「The document '...' was not found next to the IDE
installation.」。多因文档站未随包部署到 `docs\site\`，或安装目录
版本与所打开的文档页不一致。

### 帮助窗口如何前进 / 后退？

内嵌帮助窗口顶栏有 Back / Forward 导航按钮，可在已访问的文档页之间
前进后退；帮助窗口关闭后每次重新打开都会重建（不保留上次的浏览
位置）。

更多已知限制 →
[语言规格/已知限制](../language-spec/known-limitations.md)、
[VM 架构/已知限制](../vm-architecture/known-limitations.md)。

