# 三种运行方式

### 1. 在 nide 中（推荐）

1. 启动 nide（安装包内位于 `bin\` 下），菜单 文件 → 打开 → 项目...，
   选 `examples/hello_project/hello_project.nproj`。
2. 左侧「解决方案」树双击 main.n 打开编辑器。
3. 菜单 构建 → 构建项目，然后 运行 → 开始运行（F5）；
   输出窗口的「运行输出」页显示程序输出。

### 2. 命令行

在仓库根目录运行；安装包用户在安装根目录运行（ncc/nvm 位于 `bin\` 下）：

    ncc build examples/hello.n -o hello.nmod   # 编译
    nvm hello.nmod                             # 运行（退出码 42）

安装包内未设置 PATH 时写 `bin\ncc build ...`、`bin\nvm <name>.nmod`。

多文件项目用 `-p` 指定 .nproj，`-o` 指定输出位置：

    ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
    nvm hello_project.nmod

### 3. 逐例浏览

`examples/README.md` 列出全部示例与其演示主题、预期退出码。

