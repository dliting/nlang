# 概览

安装包 `bin\` 下随包分发五个工具，其中四个是命令行工具。未设置 PATH
时在命令前加 `bin\` 前缀（如 `bin\ncc build ...`）。

<!-- 本章节与 README.md Command-line Tools 节是同一事实的两个出口：命令集变更必须两侧同步，详略允许不同。 -->

| 工具 | 定位 | 参考页 |
|---|---|---|
| ncc | 编译（可立即执行） | [ncc](ncc.md) |
| nvm | 运行一个编译好的模块 | [nvm](nvm.md) |
| ndb | 调试编译后的模块 | [ndb](ndb.md) |
| ndisasm | 字节码反汇编 | [ndisasm](ndisasm.md) |
| nide | 图形 IDE（构建、运行与调试） | [三种运行方式](../getting-started/running.md)、[在 nide 中调试](../getting-started/debugging.md) |

各工具页是调用形态、标志与行为的完整参考。

## 版本查询

四个工具都用 `--version` 报版本，输出形如 `ncc (NLang) <版本>`；
nide 在 帮助 → 关于 中显示，文档站在页脚显示。
