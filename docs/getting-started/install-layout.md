# 安装与布局

两种发行形态，内容一致：

- **NSIS 安装器**（`NLang-<版本>-win64.exe`）：默认安装到 `C:\Program Files\NLang`，
  并创建开始菜单快捷方式；
- **zip 便携包**（`NLang-<版本>-win64.zip`）：解压即用，目录结构与安装器相同。

安装根目录布局：

| 目录/文件 | 内容 |
|---|---|
| `bin\` | ncc.exe、nvm.exe、ndisasm.exe、ndb.exe、nide.exe 与 Qt 运行时 |
| `examples\` | 全部示例程序，含多文件项目 `hello_project` |
| `docs\site\` | 本帮助文档站——nide 内嵌帮助窗口加载的正是它 |
| `LICENSE`、`README.md` | 许可证与项目说明 |

两点注意：

- `C:\Program Files` 对普通用户只读，而构建会把 `.nmod` 写在工程文件旁——
  在 nide 中打开示例前，先把 `examples\` 复制到可写目录。
- 可执行文件动态链接 MSVC 运行库，需要
  [VC++ Redistributable for Visual Studio 2015-2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  （装有 Visual Studio 2022 的机器通常已具备）。

