# 常见问题

### `.nmod` 写到哪里了？

`ncc build hello.n` 不带 `-o` 时，模块文件写在**当前工作目录**，
不是源文件旁边——在别的目录里找不到产物时先想到这一点。各形态的
缺省规则与优先级见[命令行工具](command-line-tools.md)。

nide 侧由输出位置优先级决定（详见
[三种运行方式](running.md#配置-nide)）：

- 工程：`.nproj` 的 `outputDir` > nide 全局构建输出目录 > 工程目录；
- 独立 `.n` 文件：全局构建输出目录 > 缺省位置（用户临时目录下的
  `nlang-nide` 子目录）。

### 退出码不是想要的值？

进程退出码就是 `main` 的返回值，Windows 保留 32 位原值；但 POSIX shell
（bash、Git-Bash、CI 的 bash 步骤）按惯例只保留低 8 位——`return 300`
在 Python/cmd/PowerShell 里看到 300，在 bash 里看到 44（对 256 取模）。
测试约定预期值 0–255，让所有观察者的视图一致。

详见 → [语言规格/退出码约定](../language-spec/exit-code-convention.md)。

### 中文输出乱码？

程序输出是 UTF-8 字节。Windows 控制台默认代码页（如中文系统的 GBK）
会把它们显示成乱码；先执行 `chcp 65001` 切换到 UTF-8 代码页再运行程序。
注意 `fs` 与 `io` 的文件路径、文件名经由系统活动代码页转换，
非 ASCII 文件名不一定能按 UTF-8 往返。

详见 → [语言规格/标准库](../language-spec/standard-library.md)。

### 帮助文档与搜索在哪？

nide 帮助菜单的「NLang 入门」「语言规格」「VM 架构」都在 IDE 内嵌的
帮助窗口中打开（内容即安装目录 `docs\site\` 下的文档站），
左侧是导航目录。搜索框在窗口左上角（站点标题旁），支持全文检索。

