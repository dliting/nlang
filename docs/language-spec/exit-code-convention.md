# 退出码约定

## main 返回值 → 进程退出码

`main` 的返回值就是进程退出码：nvm（以及 `ncc run`、`ncc <文件>` 的执行
阶段）以返回值调用 `ExitProcess` 结束进程。`ncc build` 只编译不执行，
成功时退出码恒为 0。

Windows 保留 32 位完整退出码，但**不同观察者的视图不同**。
以 `return 300;` 为例（均已实测）：

| 观察者 | 看到的值 |
|--------|----------|
| Python `subprocess`（e2e runner）、cmd 的 `%ERRORLEVEL%`、PowerShell 的 `$LASTEXITCODE` | 300 |
| POSIX shell（bash 的 `$?`、Git-Bash、CI 的 bash 步骤） | 44（300 对 256 取模，即低 8 位） |

负返回值不建议使用：Windows 侧按 32 位无符号解释（`return -1` 在
Python/PowerShell/cmd 中观察到 4294967295），POSIX shell 再截断，
两端视图都不直观。

## 工具自身的 0/1 约定

ncc 与 nvm 自身的失败一律退出 1，与程序退出码区分：

- **ncc**：用法/参数错误、编译失败（`Compilation failed.`）、编译器
  内部错误（`Compiler internal error:`）→ 1；`ncc build` 成功 → 0；
  `ncc run`/直跑模式正常结束 → `main` 的返回值，运行时错误 → 1。
- **nvm**：模块加载失败、运行时错误（含未捕获的 NLang 异常）→ 1，
  stderr 打印 `Runtime error: ...` 与调用回溯；正常运行 → `main`
  的返回值。

因此惯用法是：0 表示成功；程序自检失败用 `return 1;`（入门指南片段的
`if (条件) return <特征值>; return 1;` 护栏就是这种形状）；需要区分
多种失败时使用不同的小正数值。

## 测试纪律

- 测试的预期退出码一律取 **0–255**：同一程序在任何观察者
  （Python/cmd/PowerShell/bash）下的视图才一致。e2e 清单
  （`tests/e2e/manifest.txt`）第二列即预期退出码。
- 需要大数值时用模运算或派生值，把「自检通过」编码进小退出码，
  例如循环求和后 `if (total == 25) return 25; return 1;`。

详见 → [入门指南/常见问题](../nlang-getting-started.md)。
