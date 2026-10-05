# ncc —— 编译与执行

ncc把源码编译为字节码产物，可选立即执行。它是日常开发的
入口：写好一个`.n`想马上看结果，直接`ncc <文件>`；要产出交给
nvm运行或ndb调试的模块，用`build`形态；多文件项目交给`-p`。
构建脚本与自动化流水线同样用它。产物分两种形态：单文件编译产出
`.ncu`单元映像，项目编译产出`.npkg`程序包（见「产物与加载期
链接」）。

## 调用形态

| 形态 | 命令 | 行为 |
|---|---|---|
| 编译并执行 | `ncc <source.n> [-o out.ncu] [-I <dir>...]` | 编译后立即运行 |
| 仅编译 | `ncc build <source.n> [-o out.ncu] [-I <dir>...]` | 产出 .ncu |
| 项目：编译并执行 | `ncc -p <project.nproj> [-o out.npkg] [-I <dir>...]` | 整项目编译后运行 |
| 项目：仅编译 | `ncc build -p <project.nproj> [-o out.npkg]` | 产出 .npkg |
| 仅执行 | `ncc run <program.ncu\|.npkg>` | 等价nvm；除`-I`与`--verbose`/`-v`外的多余参数报错 |

## 标志

| 标志 | 适用形态 | 说明 |
|---|---|---|
| `-o <path>` | 形态1-4 | 输出路径——单文件形态给`.ncu`、项目形态给`.npkg`；重复给报错 |
| `-p <nproj>` | 项目形态 | 不可与源文件位置参数同用；重复给报错 |
| `-I <dir>`（或`-I<dir>`连写） | 形态1-5 | 库搜索路径，可多次给：编译期查找被import的`.n`与外部`.ncu`/`.npkg`，运行期定位闭包成员与native动态库 |
| `--verbose` / `-v` | 形态1-5 | 打印解析后的导入搜索路径——每行一个目录，括注来源层——随后照常执行（列表形态与nvm相同） |

错误与诊断信息打到stderr，`Compiled successfully:`成功行打到
stdout。常见错误形态：

```text
Error: option -o given more than once.
Error: option -o requires a value.
Error: unexpected extra argument 'extra.n'.
Error: 'examples/hello_project/hello_project.nproj' looks like a project file; use -p examples/hello_project/hello_project.nproj
```

（把 .nproj当源文件位置参数喂入时，ncc直接提示改用`-p`。）

## 产物与加载期链接

NLang的产物**不含库代码**：每个参与编译的源文件产出一幅独立的
**单元映像**，跨包符号（如`io.print`、`utils.helper.answer`）在映像里
以**导入槽**记录。执行时，加载器沿库搜索路径发现全部依赖——
`stdlib.npkg`这样的库包、外部`.ncu`——与产物一起链接成唯一的运行期
模块。nvm、`ncc run`与ncc的编译后立即执行走的都是同一条
装载＋链接路径，行为一致。

- **`.ncu`（单元映像）**——单文件编译的产物，承载入口单元。运行它
  时，它import的外部模块必须仍在搜索路径上可定位（标准库随工具链
  分发，自动找到；第三方库须`-I`指路或设`NLANG_PATH`）。
- **`.npkg`（包归档）**——项目编译的产物：项目里每个单元一个成员
  （成员名即模块路径），程序包额外携带一条入口记录。同一格式也用于
  纯库分发（`stdlib.npkg`就是成员为`io`/`math`/`fs`的库包）。

缺包时诊断一次报清，并列出已搜索的全部目录：

```text
Runtime error: nloader failed:
  module 'lib' not found (searched: <目录>, ...)
```

先检查`-I`与`NLANG_PATH`是否覆盖了依赖所在目录。

## 默认输出位置

不带`-o`时，产物位置按形态分两种：

- **单文件形态**（`ncc <source.n>` / `ncc build <source.n>`）：写在
  **当前工作目录**，文件名取源文件名主干——不是源文件旁边。在别的
  目录里找不到产物时先想到这一点。
- **`-p`形态**（`ncc -p <nproj>` / `ncc build -p <nproj>`）：写在
  **.nproj所在目录**，包名取项目名（产物为`.npkg`）；此时成功行
  显示产物的完整路径。

`.nproj`的`outputDir`属性可以重定向产物：相对路径相对项目文件解析，
绝对路径则整体替换输出目录。

## 多文件项目 .nproj

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name`是输出模块名（缺省取文件名主干），`outputDir`可选重定向
`.npkg`（相对项目文件），`File`路径相对项目文件所在目录。完整示例见
`examples/hello_project/`。

## 库搜索路径

被`import`的`.n`文件按一组有序目录查找：`-I`指定的目录排在最前，
其后是`.nproj`的`<ImportPaths>`、源文件 / 项目目录、环境变量
`NLANG_PATH`（Windows以`;`、POSIX以`:`分隔），最后是标准库目录
等系统缺省。前面的目录优先，重复目录只保留第一次出现。同一组目录在
运行期继续服务闭包装载（`.ncu`/`.npkg`成员定位）与native动态库
加载。`--verbose`（短写法`-v`）让ncc在开始编译之前打印这份解析后的顺序（每行
括注来源层，如`(-I)`、`(project import paths)`、`(local
directory)`、`(NLANG_PATH)`、`(system)`）。

`.nproj`可用`<ImportPaths>`持久化搜索目录（路径相对项目文件存储）：

```xml
<Project name="app">
  <Sources><File path="main.n"/></Sources>
  <ImportPaths><Dir path="../libs"/></ImportPaths>
</Project>
```

完整的五层顺序、`native`库以及在nide中的图形配置，见语言规格的
「库与搜索路径」。
