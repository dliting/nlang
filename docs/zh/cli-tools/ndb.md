# ndb —— 调试器

ndb 是 NLang 的交互式调试器：加载模块后停在入口，按命令设断点、
单步、查看局部变量与调用栈。程序行为与预期不符时用它定位问题；
跨文件项目调试（断点按 `文件:行` 或函数名设置）是主用例。同一
会话也以行协议形式提供给嵌入前端（`--machine`），nide 的图形
调试器就构建在其上。

```text
ndb <module.nmod>
```

加载模块后**停在入口首条语句**（等价 gdb 的 `start`），给出提示符
`(ndb) `，从 stdin 逐条读命令；stdin EOF 等同 `q`。

## 命令表

| 命令 | 长别名 | 说明 |
|---|---|---|
| `b <file.n:LINE \| LINE \| funcName>` | break | 设断点；裸 `LINE` 在当前帧的文件里解析；同名函数全部命中 |
| `i b` | info | 断点清单（含命中数） |
| `d <id>` | delete | 删除断点 |
| `c` | continue | 继续运行 |
| `s` | step | 单步进入 |
| `n` | next | 单步越过 |
| `f` | finish | 步出当前函数 |
| `bt` | backtrace | 调用栈 |
| `frame <n>` | — | 选择帧 |
| `info locals` | info | 所选帧的局部变量（隐藏名过滤） |
| `p <name>` | print | 打印一个局部变量 |
| `l [行号]` | list | 源码窗口（当前行 `->` 标记） |
| `x` | — | 所选帧反汇编（当前指令 `>>` 标记） |
| `catch on\|off` | — | throw 时中断（默认 off） |
| `help` | — | 命令帮助 |
| `q` | quit | 退出（杀掉被调试程序） |

短命令是规范形式（与 `help` 输出一致），等价的长别名同样接受。

一个完整会话（调试 `examples/hello_project`，断点按函数名设在第二个
文件里）：

```console
$ ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
Compiled successfully: hello_project.nmod
$ ndb hello_project.nmod
Stopped: main (main.n:6)
(ndb) b addBoth
Breakpoint 1 at addBoth (utils.n:2)
(ndb) c
Breakpoint 1, addBoth (utils.n:2)
(ndb) bt
#0  addBoth (utils.n:2)
#1  main (main.n:6)
(ndb) info locals
a = 40
b = 2
(ndb) p a
a = 40
(ndb) c
Program exited with code 0.
```

程序跑完时 ndb 打印 `Program exited with code N.` 并以同一个退出码
退出；`q` 或 stdin EOF 则杀掉程序、ndb 自身退出码 0。

## 嵌入前端协议

`ndb --machine <module.nmod>` 在 stdin/stdout 上暴露同一会话的
tab 分隔行协议，供嵌入前端使用——nide 的图形调试器就构建在它之上。
协议细节见[在 nide 中调试](../getting-started/debugging.md)与
[调试器架构](../vm-architecture/debugging.md)。
