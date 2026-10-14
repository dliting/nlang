# 退出码约定

## main返回值 → 进程退出码

`main`的返回值就是进程退出码：nvm（以及`ncc run`、`ncc <文件>`的执行阶段）以返回值调用`ExitProcess`结束进程。nlang没有手写`main()`：脚本模式的顶层`return`就是退出码，因为顶层语句被包裹进合成的`main()`（见[nlang](../cli-tools/nlang.md)）。`ncc build`只编译不执行，成功时退出码恒为0。

Windows保留32位完整退出码，但**不同观察者的视图不同**。以`return 300;`为例：

| 观察者 | 看到的值 |
|--------|----------|
| Python `subprocess`、cmd的`%ERRORLEVEL%`、PowerShell的`$LASTEXITCODE` | 300 |
| POSIX shell（bash的`$?`、Git-Bash、持续集成（CI，continuous integration）的bash步骤） | 44（300对256取模，即低8位） |

负返回值不建议使用：Windows侧按32位无符号解释（`return -1`在Python/PowerShell/cmd中观察到4294967295），POSIX shell再截断，两端视图都不直观。

## 工具自身的0/1约定

ncc、nvm与nlang自身的失败一律退出1，与程序退出码区分：

- **ncc**：用法/参数错误、编译失败（`Compilation failed.`）、编译器内部错误（`Compiler internal error:`）→ 1；`ncc build`成功 → 0；`ncc run`/直跑模式正常结束 → `main`的返回值，运行时错误 → 1。
- **nvm**：模块加载失败、运行时错误（含未捕获的NLang异常）→ 1，stderr打印`Runtime error: ...`与调用回溯；正常运行 → `main`的返回值。
- **nlang**：用法/参数错误、编译失败、运行时错误（含未捕获异常）→ 1（形态同nvm）；正常运行 → 顶层`return`的值；没有顶层return → 0。**空脚本（无可执行语句）退出0**。这是刻意安排，与nvm不同：nvm运行无入口的产物时退出1，而对ad-hoc运行来说，「无事可做」就是成功。

因此惯用法是：0表示成功；程序自检失败用`return 1;`（入门指南片段的`if (条件) return <特征值>; return 1;`就是这种写法）；需要区分多种失败时使用不同的小正数值。

## 测试纪律

- 测试的预期退出码一律取**0–255**：同一程序在任何观察者（Python/cmd/PowerShell/bash）下的视图才一致。
- 需要大数值时用模运算或派生值，把「自检通过」编码进小退出码，例如循环求和后`if (total == 25) return 25; return 1;`。

详见 → [入门指南/常见问题](../getting-started/faq.md)。
