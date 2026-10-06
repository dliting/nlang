# 调试支持

NLang自带**ndb**，一个命令行调试器（`ndb <program.ncu|.npkg>`）。它在进程内加载模块，跑在标准`VmExecutor`上，通过两个小接口驱动VM——
nide调试器经线路协议复用的正是同一引擎层（debugpy/dlv式的「引擎 + 轻前端」模型）。

## 钩子模型

- `IDebugHooks`（src/vm/IDebugHooks.h）——回调接口。两个检查点：
  - `OnStatement`在每条`OP_DebugInfo`之后触发（每条语句前都有一条，携带其源码行号）。
  - `OnThrow`在每个抛出点触发（`RaiseNlangException`中的内建失败、用户`throw`与`rethrow`各自的指令处），在异常对象已经存在、但**尚未**开始展开的时刻——此刻完整的NLang调用栈与每一帧的局部变量都还活着。
- `IVmDebugView`——对冻结状态的只读查询：帧数、每帧的函数名/源文件/行号/pc，以及每帧带显示字符串的局部变量。

安装钩子（`VmExecutor::SetDebugHooks`）是可选的；没有前端安装时，检查点的开销只是每条语句一次空指针测试。

## 停止语义

回调运行时程序处于冻结状态（NLang函数调用就是C++递归，因此整条栈都活在`OnStatement`内部）。不返回就保持冻结；返回即原地恢复。ndb的命令循环就跑在回调里。

语句粒度：每条语句停一次。同一行上的两条语句停两次；跨行的语句只停一次。

## 循环锚点每轮迭代都命中

循环语句把调试锚点放在回边上，因此锚点行上的断点——以及单步——**每一轮**迭代都会停下，而不是只在进入循环时（gdb语义）：条件入口对`while`/`for`而言每轮求值一次（包括结束循环的最后一次求值），`do-while`则是尾部条件。锚点就是语法为循环头记录的语句行，因此`b <file>:<loop line>`的行为与所有行导向调试器一致；`for`头部行上还带有初始化与步进语句，它们各自的锚点都计在这行的同一个断点下。

## 会话分层

在引擎钩子与前端之间是`DebugSessionController`（src/vm/DebugSessionController.h，与钩子一样是PRIVATE包含）：与前端无关的会话状态——断点表、函数断点、源码路径匹配与单步深度的状态机。执行器经`IDebugHooks`调用它；前端实现`IDebugFrontEnd`（OnStopped /
WaitUntilResume / OnExited / OnRuntimeError），经`StopInfo`载荷驱动。随包发行两个适配器：ndb的交互式命令行界面（CLI，command-line interface）与`--machine`。冻结窗口在`OnStopped`打开，在`WaitUntilResume`返回时关闭；窗口之外调用恢复命令或调试视图属于前端编程错误（`std::logic_error`）。

断点身份是**每源码行一个id**：一行携带多个语句锚点时（例如一行写两条语句），它们合并到同一个断点id之下，因此设置/删除/列举在两个前端之间无歧义且完全一致。语句级粒度不变——该id下的每个锚点仍然会停；停止报告共享id。

## 机器模式

`ndb --machine <program.ncu|.npkg>`在stdin/stdout上讲一套线路协议（完整文档见src/tools/ndb/MachineFrontEnd.h）：事件是以制表符连接、字段转义的行（`hello`/`bp`/`stopped`/`frame`/`local`/`done`/`output`/
`exited`/`error`/`err`），命令是空格分隔的裸记号（`b`/`bfunc`/`d`/
`breakthrow`/`bt`/`frame`/`locals`/`run`/`c`/`s`/`n`/`f`）。一条数据命令走同一通道：`stdin<TAB><payload>`（payload与任何字段同样转义）送达一行程序输入——识别发生在原始线路文本上，因此payload的首尾空格得以保留；它在所有读取位置都被接受（`run`之前作为提前输入排队、冻结停止期间排队而不打断停止、程序停在`io.readLine`时被实时消费），且从不应答；程序的下一次读取就是应答。会话以一段前奏开场，断点在此时预置；`run`结束前奏并开始执行——在此之前，窗口绑定命令一律应答`err`（还没有任何东西被冻结）。恢复命令应答下一次停止或退出事件；其余命令原地应答。应答形状遵循同一文法：查询命令先流出数据事件再`done`（`bt` → `frame`* + `done`、`locals` →
`local`* + `done`）；设断点命令以`bp`回执应答，其余选择与变更命令只应答`done`——`frame`事件绝不出现在`bt`应答之外，消费方因此可以每条`frame`事件追加一行栈帧而不会重复。帧编号：`stopped`的深度从1计起且恒等于帧数（一次停止冻结最内层帧），而`bt`/`frame`从0计起，最内层为0。

## 宿主I/O接缝

`IHostIo`（src/vm/IHostIo.h）把执行器的输入输出（I/O，input/output）与进程控制台解耦：输出字节经`OnOutput`原样送出；输入是可选且三态的——程序停在输入读取时由`ReadInputLine`回答行、输入结束或无通道（允许阻塞）；已安装的宿主若不覆写它，就回答「无通道」，输入读取会抛出可捕获的IOException，而不是静默消费嵌入方的流。`HasInputLine`是`io.hasInput`背后的非阻塞探询（无探询能力的宿主保持默认「可能还有输入」）。机器模式实现这条接缝，把程序输出导进`output`事件、用`stdin`数据命令供给输入读取（其探询只报告已停驻行）；没有安装宿主时（nvm、ncc、CLI前端）行为不变。三个回调都不得抛错：它们跑在执行线程上，受与钩子相同的冻结期纪律约束。

## 冻结期纪律

回调内部：

- 绝不执行NLang代码（不做`toString`分派——格式化器是浅层的，只展开一层字段/元素，嵌套引用用短标签表示）；
- 绝不在NLang堆上分配（冻结时刻堆是一致的，必须保持一致——C++分配没有问题，收集器只在执行期间的安全点运行）；
- 绝不让C++异常逃逸进VM（它们会跨越NLang的try/catch边界）——ndb的命令循环捕获一切。

引用类型值的判别与垃圾回收（GC，garbage collection）标记器相同：声明kind剪掉基本类型；数组类型字段在`.ncu`里携带声明侧的`RTK_Array`，因此声明kind是可靠的数组探测器，运行期槽位kind起佐证作用。只有Class/Struct/Func声明kind才落到运行期槽位kind；Int32/Float/
String/Array直接按声明kind显示，普通int永远不会走到引用标签路径。

## 断点寻址

每个函数记录编译所在翻译单元的路径（`CompiledFunction::sourceFile`）。`b file.n:LINE`对记录路径做后缀匹配；`b LINE`在所选帧的文件里解析；`b funcName`停在函数第一条语句。加载期链接器把每个单元连同它的`sourceFile`与`locals`记录一起合并，因此被导入函数可以按断点寻址、其帧可以检查。

## 已知限制

没有条件断点与监视点；空函数体的`b func`永不命中；函数名裸显（无`Class.method`限定）；抛出停止处不把异常对象暴露为伪变量；不能附加到已运行进程；native调用透明直通；抛出停止锚定在语句pc的近似值上；pc值是16位字节码偏移（执行器既有的`uint16_t opPc`——超过64 KiB字节码的函数会回绕；这是既有的VM上限，不是调试器限制）；共享`.ncu`可能携带过期的源码路径（ndb回退到`.ncu`所在目录，再退化为`l`只显示行号）。集成开发环境（IDE，integrated development environment）会话继承这些限制并另加若干面向用户的限制——每会话一份行号快照（不支持会话中编辑/重建）、停止即硬终止——记录在入门手册的调试指南：[在nide中调试](../getting-started/debugging.md)。
