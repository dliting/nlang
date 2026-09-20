# 命令行工具

安装包 `bin\` 下随包分发五个工具，其中四个是命令行工具：`ncc`（编译）、
`nvm`（运行）、`ndb`（调试）、`ndisasm`（反汇编）；第五个是 nide 图形
IDE，见[三种运行方式](running.md)。本页是四个命令行工具的完整参考。
未设置 PATH 时在命令前加 `bin\` 前缀（如 `bin\ncc build ...`）。

<!-- 本页与 README.md Command-line Tools 节是同一事实的两个出口：命令集变更必须两侧同步，详略允许不同。 -->

## ncc —— 编译与执行

### 调用形态

| 形态 | 命令 | 行为 |
|---|---|---|
| 编译并执行 | `ncc <source.n> [-o out.nmod] [-I <dir>...]` | 编译后立即运行 |
| 仅编译 | `ncc build <source.n> [-o out.nmod] [-I <dir>...]` | 产出 .nmod |
| 项目：编译并执行 | `ncc -p <project.nproj> [-o out.nmod] [-I <dir>...]` | 整项目编译后运行 |
| 项目：仅编译 | `ncc build -p <project.nproj> [-o out.nmod]` | 产出 .nmod |
| 仅执行 | `ncc run <module.nmod>` | 等价 nvm，多余参数被忽略 |

### 标志

| 标志 | 适用形态 | 说明 |
|---|---|---|
| `-o <path>` | 形态 1-4 | 输出 .nmod 路径；重复给报错 |
| `-p <nproj>` | 项目形态 | 不可与源文件位置参数同用；重复给报错 |
| `-I <dir>`（或 `-I<dir>` 连写） | 形态 1-4 | .nmod 导入搜索路径，可多次给 |

错误与诊断信息打到 stderr，`Compiled successfully:` 成功行打到
stdout。常见错误形态：

```text
Error: option -o given more than once.
Error: option -o requires a value.
Error: unexpected extra argument 'extra.n'.
Error: 'examples/hello_project/hello_project.nproj' looks like a project file; use -p examples/hello_project/hello_project.nproj
```

（把 .nproj 当源文件位置参数喂入时，ncc 直接提示改用 `-p`。）

### 默认输出位置

不带 `-o` 时，产物位置按形态分两种：

- **单文件形态**（`ncc <source.n>` / `ncc build <source.n>`）：写在
  **当前工作目录**，文件名取源文件名主干——不是源文件旁边。在别的
  目录里找不到产物时先想到这一点。
- **`-p` 形态**（`ncc -p <nproj>` / `ncc build -p <nproj>`）：写在
  **.nproj 所在目录**，模块名取项目名；此时成功行显示产物的完整路径。

`.nproj` 的 `outputDir` 属性可以重定向产物：相对路径相对项目文件解析，
绝对路径则整体替换输出目录。

### 多文件项目 .nproj

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project" namespace="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` 是输出模块名（缺省取文件名主干），`outputDir` 可选重定向
`.nmod`（相对项目文件），`File` 路径相对项目文件所在目录。完整示例见
`examples/hello_project/`。

## nvm —— 运行模块

```text
nvm <module.nmod> [--gc-stress=N]
```

运行一个编译好的模块，进程退出码 = `main` 返回值（约定详见
[退出码约定](../language-spec/exit-code-convention.md)）。模块打不开
时报 `Runtime error: Failed to open module file: <路径>`。`--gc-stress=N`
是测试旋钮：把两套 GC 阈值钳到极小值，任何漏追踪的引用会在几次分配
内变悬垂——用于验证内存管理变更，日常使用不需要。

## ndb —— 调试器

```text
ndb <module.nmod>
```

加载模块后**停在入口首条语句**（等价 gdb 的 `start`），给出提示符
`(ndb) `，从 stdin 逐条读命令；stdin EOF 等同 `q`。

### 命令表

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

短形是规范命令面（与 `help` 输出一致），长别名等价接受。

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

### 嵌入前端协议

`ndb --machine <module.nmod>` 在 stdin/stdout 上暴露同一会话的
tab 分隔行协议，供嵌入前端使用——nide 的图形调试器就构建在它之上。
协议细节见[在 nide 中调试](debugging.md)与
[调试器架构](../../vm-architecture/debugging.md)。

## ndisasm —— 反汇编

```text
ndisasm <module.nmod>
ndisasm -func <name> <module.nmod>
```

两种调用：全量转储，或 `-func <name>` 只保留一个函数节（其余节仍在）。
**`-func` 必须写在模块路径前面**——放在后面会被静默忽略，输出与全量
转储相同。输出节顺序固定：`module:` → `structs:` → `classes:` →
`string constants:` → 逐个函数节。

函数头一行给出全部元数据（`file=` 镜像编译时传入的源路径形态；内建
函数带 `intrinsic=N` 而无 `file=`）：

```text
function main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
  bytecode:
    0000: debug 2
    0003: const_i32 42
    0008: assign 0
    ...
```

全量转储会列出全部内建类方法（均 `(no bytecode)`，一个 hello 也有
七十余个），所以日常看单个函数用 `-func` 过滤。典型用途：对照 ndb
的 `x` 命令核对指令地址、检查优化结果、排查序列化问题。

## 版本查询

四个工具都用 `--version` 报版本，输出形如 `ncc (NLang) <版本>`；
nide 在 帮助 → 关于 中显示，文档站在页脚显示。
