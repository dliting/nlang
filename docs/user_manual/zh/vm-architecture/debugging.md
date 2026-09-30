# 调试支持

NLang 自带 **ndb**，一个命令行调试器（`ndb <module.nmod>`）。它在进
程内加载模块，跑在标准 `VmExecutor` 上，通过两个小接口驱动 VM——
nide 调试器经线路协议复用的正是同一引擎层（debugpy/dlv 式的
「引擎 + 轻前端」模型）。

## 钩子模型

- `IDebugHooks`（src/vm/IDebugHooks.h）——回调接口。两个检查点：
  - `OnStatement` 在每条 `OP_DebugInfo` 之后触发（每条语句前都有一
    条，携带其源码行号）。
  - `OnThrow` 在每个抛出点触发（`RaiseNlangException` 中的内建失
    败、用户 `throw` 与 `rethrow` 各自的指令处），在异常对象已经存
    在、但**尚未**开始展开的时刻——此刻完整的 NLang 调用栈与每一帧
    的局部变量都还活着。
- `IVmDebugView`——对冻结状态的只读查询：帧数、每帧的函数名/源文
  件/行号/pc，以及每帧带显示字符串的局部变量。

安装钩子（`VmExecutor::SetDebugHooks`）是可选的；没有前端安装时，
检查点的开销只是每条语句一次空指针测试。

## 停止语义

回调运行时程序处于冻结状态（NLang 函数调用就是 C++ 递归，因此整条
栈都活在 `OnStatement` 内部）。不返回就保持冻结；返回即原地恢复。
ndb 的命令循环就跑在回调里。

语句粒度：每条语句停一次。同一行上的两条语句停两次；跨行的语句只停
一次。

## 循环锚点每轮迭代都命中

循环语句把调试锚点放在回边上，因此锚点行上的断点——以及单步——
**每一轮**迭代都会停下，而不是只在进入循环时（gdb 语义）：条件入口
对 `while`/`for` 而言每轮求值一次（包括结束循环的最后一次求值），
`do-while` 则是尾部条件。锚点就是语法为循环头记录的语句行，因此
`b <file>:<loop line>` 的行为与所有行导向调试器一致；`for` 头部行上
还带有初始化与步进语句，它们各自的锚点都计在这行的同一个断点下。

## 会话分层

在引擎钩子与前端之间是 `DebugSessionController`（src/vm/
DebugSessionController.h，与钩子一样是 PRIVATE 包含）：与前端无关的
会话状态——断点表、函数断点、源码路径匹配与单步深度的状态机。执行
器经 `IDebugHooks` 调用它；前端实现 `IDebugFrontEnd`（OnStopped /
WaitUntilResume / OnExited / OnRuntimeError），经 `StopInfo` 载荷驱
动。随包发行两个适配器：ndb 的交互式 CLI 与 `--machine`。冻结窗口在
`OnStopped` 打开，在 `WaitUntilResume` 返回时关闭；窗口之外调用恢复
命令或调试视图属于前端编程错误（`std::logic_error`）。

断点身份是**每源码行一个 id**：一行携带多个语句锚点时（例如一行写
两条语句），它们合并到同一个断点 id 之下，因此设置/删除/列举在两个
前端之间无歧义且完全一致。语句级粒度不变——该 id 下的每个锚点仍然
会停；停止报告共享 id。

## 机器模式

`ndb --machine <module.nmod>` 在 stdin/stdout 上讲一套线路协议（完
整文档见 src/tools/ndb/MachineFrontEnd.h）：事件是以制表符连接、字段
转义的行（`hello`/`bp`/`stopped`/`frame`/`local`/`done`/`output`/
`exited`/`error`/`err`），命令是空格分隔的裸记号（`b`/`bfunc`/`d`/
`breakthrow`/`bt`/`frame`/`locals`/`run`/`c`/`s`/`n`/`f`）。会话以
一段前奏开场，断点在此时预置；`run` 结束前奏并开始执行——在此之前，
窗口绑定命令一律应答 `err`（还没有任何东西被冻结）。恢复命令应答下
一次停止或退出事件；其余命令原地应答。帧编号：`stopped` 的深度从 1
计起且恒等于帧数（一次停止冻结最内层帧），而 `bt`/`frame` 从 0 计
起，最内层为 0。

## 宿主 I/O 接缝

`IHostIo`（src/vm/IHostIo.h）把执行器的 I/O 与进程控制台解耦：输出
字节经 `OnOutput` 原样送出；输入是可选的——已安装的宿主若不覆写
`IsInputAvailable()`，`io.readLine` 会抛出可捕获的 IOException，而
不是静默消费嵌入方的流。机器模式实现这条接缝，把程序输出导进
`output` 事件、把 stdin 留作协议通道；没有安装宿主时（nvm、ncc、
CLI 前端）行为不变。`OnOutput` 不得抛错：它跑在执行线程上，受与钩子
相同的冻结期纪律约束。

## 冻结期纪律

回调内部：

- 绝不执行 NLang 代码（不做 `toString` 分派——格式化器是浅层的，只
  展开一层字段/元素，嵌套引用用短标签表示）；
- 绝不在 NLang 堆上分配（冻结时刻堆是一致的，必须保持一致——C++
  分配没有问题，收集器只在执行期间的安全点运行）；
- 绝不让 C++ 异常逃逸进 VM（它们会跨越 NLang 的 try/catch 边界）
  ——ndb 的命令循环捕获一切。

引用类型值的判别与 GC 标记器相同：声明 kind 剪掉基本类型；数组类型字段
在 `.nmod` 里携带声明侧的 `RTK_Array`，因此声明
kind 是可靠的数组探测器，运行期槽位 kind 起佐证作用。只有
Class/Struct/Func 声明 kind 才落到运行期槽位 kind；Int32/Float/
String/Array 直接按声明 kind 渲染，普通 int 永远不会走到引用标签路
径。

## 断点寻址

每个函数记录编译所在翻译单元的路径（`CompiledFunction::sourceFile`）。
`b file.n:LINE` 对记录路径做后缀匹配；`b LINE` 在所选帧的文件里解
析；`b funcName` 停在函数第一条语句。导入合并会拷贝 `sourceFile` 与
`locals`，因此被导入函数可以按断点寻址、其帧可以检查。

## 已知限制

没有条件断点与监视点；空函数体的 `b func` 永不命中；函数名裸显（无
`Class.method` 限定）；抛出停止处不把异常对象暴露为伪变量；不能附加
到已运行进程；原生调用透明直通；抛出停止锚定在语句 pc 的近似值上；
pc 值是 16 位字节码偏移（执行器既有的 `uint16_t opPc`——超过
64 KiB 字节码的函数会回绕；这是既有的 VM 上限，不是调试
器限制）；共享 `.nmod` 可能携带过期的源码路径（ndb 回退到 `.nmod`
所在目录，再退化为 `l` 只显示行号）。IDE 会话继承这些限制并另加若干
面向用户的限制——调试会话内没有标准输入、每会话一份行号快照（不支
持会话中编辑/重建）、停止即硬终止——记录在入门手册的调试指南：
[在 nide 中调试](../getting-started/debugging.md)。
