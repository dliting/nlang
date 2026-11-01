> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 9.3 程序的进入与退出

程序有一个入口函数：显式的`int main()`，或从脚本体合成的`int main()`（§3.2）。执行从调用它开始；它不带参数。

入口返回的值成为进程退出状态。实现把入口的32位返回值原样传给宿主；宿主如何把它呈现给观察者是宿主行为，不是语言行为：POSIX shell把值截断到低8位（`return 300;`被观察为`44`），而cmd、PowerShell与Python观察到完整的无符号32位值（`return -1;`被观察为`4294967295`）。退出状态会被其他工具检查的程序应当保持在`0`到`255`之内。

到达入口函数末尾会返回`int`的默认值，因此程序以状态`0`退出（§6.6）。未捕获的异常使程序以状态`1`结束（§8.1），宿主级执行期错误（§7.1）同样使程序以状态`1`结束。

### 工具返回什么

*资料性：*`main`的返回值就是进程退出码：nvm（以及`ncc run`、`ncc <文件>`的执行阶段）把该值交给操作系统以结束进程。nlang的脚本没有手写`main()`：顶层`return`就是退出码，因为顶层语句被包裹进合成的`main()`。自带`main()`的普通程序用nlang照常运行（见[nlang](../../cli-tools/nlang.md)）。`ncc build`只编译不执行，成功时退出码恒为0。

不同观察者对同一退出状态看到的值不同：

| 观察者 | 看到的值 |
|--------|----------|
| Python `subprocess`、cmd的`%ERRORLEVEL%`、PowerShell的`$LASTEXITCODE` | 完整的无符号32位值 |
| POSIX shell（bash的`$?`、Git Bash、持续集成（CI，continuous integration）的bash步骤） | 低8位 |

负返回值不建议使用：无符号视图与截断视图都不直观。

ncc、nvm与nlang自身的失败一律退出1，与程序退出码区分：

- **ncc**：用法/参数错误、编译失败（`Compilation failed.`）、编译器内部错误（`Compiler internal error:`）→ 1；`ncc build`成功 → 0；`ncc run`/直跑模式正常结束 → `main`的返回值，运行时错误 → 1。
- **nvm**：模块加载失败、运行时错误（含未捕获的NLang异常）→ 1，stderr打印`Runtime error: ...`与调用回溯；正常运行 → `main`的返回值。
- **nlang**：用法/参数错误、编译失败、运行时错误（含未捕获异常）→ 1（形态同nvm）；正常运行 → 顶层`return`的值（自带`main()`的程序则是`main()`的返回值）；没有顶层return → 0。**空脚本（无可执行语句）退出0**。这是刻意安排，与nvm不同：nvm运行无入口的产物时退出1，而对ad-hoc运行来说，「无事可做」就是成功。

惯用法由此而来：0表示成功；程序自检失败用`return 1;`（入门指南片段的`if (条件) return <特征值>; return 1;`就是这种写法）；需要区分多种失败时使用不同的小正数值。

### 测试纪律

*资料性：*测试的预期退出码一律取**0–255**：只有这样才能让同一程序在任何观察者（Python/cmd/PowerShell/bash）下呈现同样的值。需要大数值时用模运算或派生值，把「自检通过」编码进小退出码——例如循环求和后`if (total == 25) return 25; return 1;`。

另见[入门指南/常见问题](../../getting-started/faq.md)。
