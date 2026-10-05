# 概览

命令行工具覆盖编译、运行、调试与反汇编全流程，适合脚本化构建、自动化测试与无图形界面的环境；偏好图形界面的开发方式见nide（[三种运行方式](../getting-started/running.md)）。

安装包`bin\`下随包分发五个工具，其中四个是命令行工具。未设置PATH时在命令前加`bin\`前缀（如`bin\ncc build ...`）。

<!-- 本章节与README.md Command-line Tools节是同一事实的两个出口：命令集变更必须两侧同步，详略允许不同。 -->

| 工具 | 定位 | 参考页 |
|---|---|---|
| ncc | 编译（可立即执行） | [ncc](ncc.md) |
| nvm | 运行一个编译好的模块 | [nvm](nvm.md) |
| ndb | 调试编译后的模块 | [ndb](ndb.md) |
| ndisasm | 字节码反汇编 | [ndisasm](ndisasm.md) |
| nide | 图形集成开发环境（IDE，integrated development environment）（构建、运行与调试） | [三种运行方式](../getting-started/running.md)、[在nide中调试](../getting-started/debugging.md) |

各工具页是调用形态、标志与行为的完整参考。

## 版本查询

四个工具都用`--version`报版本，输出形如`ncc (NLang) <版本>`；nide在帮助 → 关于中显示，文档站在页脚显示。

## 编码约定

四个命令行工具以Unicode转换格式（UTF-8，Unicode Transformation Format）为进程编码（工具内嵌清单声明，Windows 10 1903+生效）：命令行参数与文件路径接受完整Unicode，诊断输出为UTF-8字节，启动时顺带把所在控制台切换到UTF-8代码页，默认控制台可直接显示非美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）文本。重定向到文件或管道时不改动输出字节。

输入侧同样是UTF-8：`.n`源文件与`.nproj`项目文件在词法前先做校验——无效字节被明确拒绝（`not valid UTF-8 (first
invalid byte at line N)`），UTF-16保存的文件得到专门提示，文件开头的UTF-8字节顺序标记（BOM，byte order mark）被接受并跳过。
