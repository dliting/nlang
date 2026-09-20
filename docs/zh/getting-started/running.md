# 三种运行方式

### 1. 在 nide 中（推荐）

1. 启动 nide（安装包内位于 `bin\` 下），菜单 文件 → 打开 → 项目...，
   选 `examples/hello_project/hello_project.nproj`。
2. 左侧「解决方案」树双击 main.n 打开编辑器。
3. 菜单 构建 → 构建项目，然后 运行 → 开始运行（Ctrl+F5）；
   输出窗口的「运行输出」页显示程序输出。F5 启动的是调试会话，
   见[在 nide 中调试](debugging.md)。

### 配置 nide

菜单 工具 → 选项 打开设置对话框，两组设置：

- **语言**：跟随系统语言 / 中文 / English。切换后**重启 nide 生效**
  （保存后立即弹出重启提示）。帮助菜单按当前语言选择对应文档树，
  页面缺失时回退另一树。
- **全局构建输出目录**：留空时用缺省位置——用户临时目录下的
  `nlang-nide` 子目录（输入框占位符显示的就是它；**占位符只是提示，
  字段值不做环境变量展开**——不要往框里填 `%TEMP%\xxx` 字面量）。
  设置后从下一次构建起生效，优先级：工程 `.nproj` 的 `outputDir` >
  此目录 > 工程目录；独立 `.n` 文件直接用此目录。

### 2. 命令行

在仓库根目录运行；安装包用户在安装根目录运行（ncc/nvm 位于 `bin\` 下）：

    ncc build examples/hello.n -o hello.nmod   # 编译
    nvm hello.nmod                             # 运行（退出码 42）

安装包内未设置 PATH 时使用 `bin\ncc build ...`、`bin\nvm <name>.nmod`。

多文件项目用 `-p` 指定 .nproj，`-o` 指定输出位置：

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
    nvm hello_project.nmod

完整参考（全部标志、默认输出位置、ndb 调试器与 ndisasm）见
[命令行工具](../cli-tools/overview.md)一章。

### 3. 逐例浏览

`examples/README.md` 列出全部示例与其演示主题、预期退出码。

