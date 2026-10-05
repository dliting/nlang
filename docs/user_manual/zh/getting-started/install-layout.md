# 安装与布局

两种发行形态，内容一致：

- **NSIS安装器**（`NLang-<版本>-win64.exe`）：默认安装到`C:\Program Files\NLang`，并创建开始菜单快捷方式；
- **zip便携包**（`NLang-<版本>-win64.zip`）：解压即用，目录结构与安装器相同。

安装根目录布局：

| 目录/文件 | 内容 |
|---|---|
| `bin\` | ncc.exe、nvm.exe、ndisasm.exe、ndb.exe、nide.exe、Qt运行时与标准库的native动态库（`nlang_io.dll`等） |
| `stdlib\` | 标准库：声明源`io.n`/`math.n`/`fs.n`与编译好的`stdlib.npkg`库包（运行期按依赖闭包装载，从这里解析） |
| `examples\` | 全部示例程序，含多文件项目`hello_project` |
| `docs\site\` | 本帮助文档站——nide内嵌帮助窗口加载的正是它 |
| `LICENSE`、`README.md` | 许可证与项目说明 |

两点注意：

- `C:\Program Files`对普通用户只读，而项目构建会把`.npkg`写在项目文件旁——在nide中打开示例前，先把`examples\`复制到可写目录（独立`.n`文件的产物落在每用户临时区，不会写进安装目录）。
- 可执行文件动态链接MSVC运行库，需要[VC++ Redistributable for Visual Studio 2015-2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)（装有Visual Studio 2022的机器通常已具备）。

