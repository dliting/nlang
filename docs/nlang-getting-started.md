# NLang 入门

NLang 是一门从 EN 游戏引擎脚本语言独立出来的教学/研究型脚本语言，
自带编译器（ncc）、字节码虚拟机（nvm）与 IDE（nide）。

## 语言概览

| 主题 | 要点 | 示例 |
|---|---|---|
| 类与对象 | `class`/构造器/`toString` | examples/class_basic.n |
| 继承 | `extends`/`super()`/虚分派 | examples/inheritance_super.n |
| 异常 | `try`/`catch`/`finally`/`throw` | examples/exceptions.n |
| 集合 | `List<T>`/`Dict<K,V>` | examples/list_tour.n、dict_tour.n |
| 数组 | 定长数组/`[...]` 初始化器 | examples/array_initlist.n |
| 字符串 | `"${}"` 插值与拼接 | examples/interpolation_tour.n |
| 转换 | `as` 拆箱/引用下转 | examples/cast_operator.n |
| 常量与断言 | `const`/`assert` | examples/const_assert.n |
| 多返回值 | `out` 参数 | examples/out_params.n |

完整语义见帮助菜单的「语言规格」。

## 三种运行方式

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

## 下一步

- 帮助菜单「语言规格」：完整语法与语义（在浏览器中打开文档站对应页面）。
- 帮助菜单「VM 架构」：字节码与执行引擎设计（同上）。
