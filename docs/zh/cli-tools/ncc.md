# ncc —— 编译与执行

ncc 把源码编译为 `.nmod` 字节码模块，可选立即执行。它是日常开发的
入口：写好一个 `.n` 想马上看结果，直接 `ncc <文件>`；要产出交给
nvm 运行或 ndb 调试的模块，用 `build` 形态；多文件项目交给 `-p`。
构建脚本与自动化流水线同样用它。

## 调用形态

| 形态 | 命令 | 行为 |
|---|---|---|
| 编译并执行 | `ncc <source.n> [-o out.nmod] [-I <dir>...]` | 编译后立即运行 |
| 仅编译 | `ncc build <source.n> [-o out.nmod] [-I <dir>...]` | 产出 .nmod |
| 项目：编译并执行 | `ncc -p <project.nproj> [-o out.nmod] [-I <dir>...]` | 整项目编译后运行 |
| 项目：仅编译 | `ncc build -p <project.nproj> [-o out.nmod]` | 产出 .nmod |
| 仅执行 | `ncc run <module.nmod>` | 等价 nvm，多余参数被忽略 |

## 标志

| 标志 | 适用形态 | 说明 |
|---|---|---|
| `-o <path>` | 形态 1-4 | 输出 .nmod 路径；重复给报错 |
| `-p <nproj>` | 项目形态 | 不可与源文件位置参数同用；重复给报错 |
| `-I <dir>`（或 `-I<dir>` 连写） | 形态 1-4 | .nmod 导入搜索路径，可多次给 |

错误与诊断信息打到 stderr，`Compiled successfully:` 成功行打到
stdout。常见错误形态：

```text
Error: option -o given more than once.
Error: option -o requires a value.
Error: unexpected extra argument 'extra.n'.
Error: 'examples/hello_project/hello_project.nproj' looks like a project file; use -p examples/hello_project/hello_project.nproj
```

（把 .nproj 当源文件位置参数喂入时，ncc 直接提示改用 `-p`。）

## 默认输出位置

不带 `-o` 时，产物位置按形态分两种：

- **单文件形态**（`ncc <source.n>` / `ncc build <source.n>`）：写在
  **当前工作目录**，文件名取源文件名主干——不是源文件旁边。在别的
  目录里找不到产物时先想到这一点。
- **`-p` 形态**（`ncc -p <nproj>` / `ncc build -p <nproj>`）：写在
  **.nproj 所在目录**，模块名取项目名；此时成功行显示产物的完整路径。

`.nproj` 的 `outputDir` 属性可以重定向产物：相对路径相对项目文件解析，
绝对路径则整体替换输出目录。

## 多文件项目 .nproj

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project" namespace="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` 是输出模块名（缺省取文件名主干），`outputDir` 可选重定向
`.nmod`（相对项目文件），`File` 路径相对项目文件所在目录。完整示例见
`examples/hello_project/`。
