# ndb —— 调试器

ndb是NLang的交互式调试器：装载程序后停在入口，按命令设断点、单步、查看局部变量与调用栈。程序行为与预期不符时用它定位问题；跨文件项目调试（断点按`文件:行`或函数名设置）是主用例。同一会话也以行协议形式提供给嵌入前端（`--machine`），nide的图形调试器就构建在其上。

```text
ndb <program.ncu|.npkg> [-I <dir>...] [--verbose | -v]
```

装载程序后**停在入口首条语句**（等价gdb的`start`），给出提示符`(ndb) `，从stdin逐条读命令；stdin文件结束（EOF，end of file）等同`q`。

## 命令表

| 命令 | 长别名 | 说明 |
|---|---|---|
| `b <file.n:LINE \| LINE \| funcName>` | break | 设断点；裸`LINE`在当前帧的文件里解析；同名函数全部命中 |
| `i b` | info | 断点清单（含命中数） |
| `d <id>` | delete | 删除断点 |
| `c` | continue | 继续运行 |
| `s` | step | 单步进入 |
| `n` | next | 单步越过 |
| `f` | finish | 步出当前函数 |
| `threads` | — | 列出全部上下文（状态/位置） |
| `t <id>` | thread | 切换检查上下文 |
| `bt` | backtrace | 调用栈 |
| `frame <n>` | — | 选择帧 |
| `info locals` | info | 所选帧的局部变量（合成名过滤；接收者以`this`显示） |
| `p <name>` | print | 打印一个局部变量 |
| `l [行号]` | list | 源码窗口（当前行`->`标记） |
| `x` | — | 所选帧反汇编（当前指令`>>`标记） |
| `catch on\|off` | — | throw时中断（默认off） |
| `help` | — | 命令帮助 |
| `q` | quit | 退出（杀掉被调试程序） |

短命令是规范形式（与`help`输出一致），等价的长别名同样接受。

一个完整会话（调试`examples/hello_project`，断点按限定函数名设在第二个文件里）：

```console
$ ncc build -p examples/hello_project/hello_project.nproj -o hello_project.npkg
Output package file: hello_project.npkg ...
Compiled successfully: hello_project.npkg
$ ndb hello_project.npkg
Stopped: main.main (main.n:6)
(ndb) b utils.addBoth
Breakpoint 1 at utils.addBoth (utils.n:2)
(ndb) c
Breakpoint 1, utils.addBoth (utils.n:2)
(ndb) bt
#0  utils.addBoth (utils.n:2)
#1  main.main (main.n:6)
(ndb) info locals
a = 40
b = 2
(ndb) p a
a = 40
(ndb) c
Program exited with code 0.
```

（函数名按模块路径限定——`b utils.addBoth`；拼写规则见语言规格的[包](../language-spec/packages.md)。）

程序跑完时ndb打印`Program exited with code N.`并以同一个退出码退出；`q`或stdin EOF则杀掉程序、ndb自身退出码0。

## 多上下文调试

`thread.spawn`派生的并发上下文同样受调试器管辖：任何上下文命中断点或throw都会全停（所有载体冻结）。`threads`每个上下文一行：焦点标记（`*`）、编号、调度状态（被执行窗口持有时带`@载体`后缀——正在执行与排队等待分派由此区分）、最内层帧位置；已结束的上下文没有帧，只显示编号与状态。`t <id>`把检查焦点切到指定上下文，`bt`/`frame`/`info locals`/`p`/`l`/`x`随之作用于该上下文，停泊中的帧（如等待通道接收）照常可查；每次停机焦点自动回到停机上下文。步进命令始终以停机上下文为目标——`t`只重定向检查，不改变步进对象。

```console
$ ndb dbg_threads.ncu
Stopped: dbg_threads.main (dbg_threads.n:11)
(ndb) b 6
Breakpoint 1 at run (dbg_threads.n:6)
(ndb) c
Breakpoint 1, run (dbg_threads.n:6)
(ndb) threads
  0  parked-chan-recv  dbg_threads.main (dbg_threads.n:14)
* 1  runnable@0  run (dbg_threads.n:6)
(ndb) t 0
* 0  parked-chan-recv  dbg_threads.main (dbg_threads.n:14)
(ndb) bt
#0  dbg_threads.main (dbg_threads.n:14)
(ndb) c
Program exited with code 9.
```

（断点设在派生上下文执行的`run`里；命中时main停在空通道的`recv`上——`threads`把它列为`parked-chan-recv`，`run`为`runnable@0`（正被0号载体持有）。`t 0`把检查焦点切到main后，`bt`读出它的停泊帧。）

## 嵌入前端协议

`ndb --machine <program.ncu|.npkg> [-I <dir>...] [--verbose | -v]`在stdin/stdout上暴露同一会话的tab分隔行协议，供嵌入前端使用——nide的图形调试器就构建在它之上。协议细节见[在nide中调试](../getting-started/debugging.md)与[调试器架构](../vm-architecture/debugging.md)。

交互式与`--machine`两种形态都接受`-I <dir>`（可多次指定），用于定位闭包成员与native动态库；搜索目录的拼接顺序与nvm相同：`-I` → 模块目录 → `NLANG_PATH` → 可执行文件目录 / 当前目录 → 标准库目录（`stdlib.npkg`所在处）。

`--verbose`（短写法`-v`）在会话开始前打印解析后的搜索路径（每行一个目录并括注来源层，同nvm），随后照常进行。`--machine`形态下这份列表输出到**stderr**——stdout是协议通道，不得混入非协议行。
