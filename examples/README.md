# NLang 示例索引

每个示例聚焦一个主题，可独立编译运行。三种运行方式见
`docs/nlang-getting-started.md`（nide 帮助菜单 → NLang 入门）。

命令行方式（在仓库根目录；安装包用户在安装根目录运行，ncc/nvm 位于 `bin\` 下）：

    ncc build examples/<name>.n -o <name>.nmod
    nvm <name>.nmod

安装包内无 PATH 时写 `bin\ncc build ...`、`bin\nvm <name>.nmod`。

`stdlib_io` 需要一行 stdin：`nvm stdlib_io.nmod < examples/stdlib_io.stdin`。
成功运行的退出码列在「退出码」列（Windows 限 0-255）。

| 示例 | 演示 | 退出码 |
|---|---|---|
| hello.n | 最小程序 | 42 |
| add.n | 函数与返回值 | 42 |
| class_basic.n | class 字段/构造器/方法/toString | 3 |
| inheritance_super.n | extends/super()/覆盖/虚分派 | 4 |
| exceptions.n | throw/try/catch/finally/内建异常/用户子类 | 5 |
| list_tour.n | `List<T>`/add/contains/indexOf/foreach | 6 |
| dict_tour.n | `Dict<K,V>`/containsKey/keys()/foreach | 7 |
| array_initlist.n | 定长数组/`[...]`初始化器/length | 8 |
| interpolation_tour.n | `"${}"`插值/toString 隐式转换 | 9 |
| cast_operator.n | as 拆箱与引用下转（数值转换走隐式赋值） | 10 |
| const_assert.n | const 局部/assert/增量赋值 | 11 |
| out_params.n | out 参数多返回值 | 12 |
| switch_tour.n | switch/enum 分支 | 0 |
| enum_methods.n | enum 方法与 this | 0 |
| delegates_tour.n | 函数/方法代理 | 7 |
| aliases_tour.n | 类型别名 | 0 |
| stdlib_string.n | string 方法 | 0 |
| stdlib_math.n | math 命名空间 | 0 |
| stdlib_io.n | io 控制台与文件内容（需 stdin） | 0 |
| stdlib_fs.n | fs 路径/目录/元数据 | 0 |
| hello_project/ | 多文件项目（nide 打开 .nproj） | 0 |
