# NLang

**中文** | [English](README.md)

NLang 是一门面向嵌入与自动化场景的静态类型脚本语言——提供精简的
C++ 宿主 API、原生绑定与进程内调试钩子——同时也是 AI 友好语言特性的
试验台。它自带编译器、字节码虚拟机、调试器与 IDE，作为一个开放的
教学/研究项目开发。

![NLang IDE（nide）](docs/images/nide-overview.png)

NLang IDE：左侧解决方案树（项目与独立 `.n` 文件）、中部编辑器、
下方构建与运行输出。界面语言默认跟随系统区域设置，可在
工具 → 选项 中固定为中文或英文（重启后生效）。

## 快速开始

部署和学习 NLang 最快的途径是预构建的 Windows 发布包——无需任何
工具链，无需构建：

1. **下载** 安装程序 `NLang-<version>-win64.exe`（或便携版
   `NLang-<version>-win64.zip`），见
   [releases 页面](https://github.com/dliting/nlang/releases)。
2. **安装** —— 运行安装程序（默认安装到 `C:\Program Files\NLang`，
   并为 IDE 添加开始菜单项；会有 UAC 提示）。不想安装？把便携版
   压缩包解压到任意可写目录，运行 `bin\nide.exe` 即可。
3. **阅读手册** —— 启动 **NLang IDE** 并打开其 **帮助菜单**：
   完整的文档站随包分发，完全离线可用。「入门」章节按主题逐页
   讲解语言、命令行与 IDE，并附可运行的示例程序。
4. **跑点什么** —— 在 IDE 中打开自带的 `examples/` 并按运行
   （先复制到可写目录——构建会在源文件旁写 `.nmod`）。
   `examples/README.md` 索引了全部示例。

可执行文件动态链接 MSVC 运行时——若机器上没有 Visual Studio 2022，
请安装 [VC++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)。

想改从源码构建？本页其余部分介绍构建。

## 构建依赖

| 依赖 | 版本 | 必需 | 说明 |
|------------|---------|----------|-------|
| CMake | 3.16+ | 是 | 构建系统 |
| C++ 编译器 | C++17 | 是 | MSVC 19.44+ / GCC 9+ / Clang 10+ |
| Flex | 2.6+ | 是（开发） | 词法分析器生成器。Windows：[win_flex_bison](https://github.com/lexxmark/winflexbison) |
| Bison | 3.0+ | 是（开发） | 语法分析器生成器。Windows：[win_flex_bison](https://github.com/lexxmark/winflexbison) |
| Python3 | 3.13+ | 可选 | 构建脚本。Windows 上推荐 conda `py313` 环境 |
| Qt5 | 5.15+ | 仅 IDE | Core、Xml、Widgets 模块 |
| LLVM | 15+ | 可选 | 代码生成后端（`-DNLANG_ENABLE_LLVM=ON`） |

### Windows 开发环境配置

1. 安装 Visual Studio 2022（含 C++17 支持）
2. 下载 [win_flex_bison](https://github.com/lexxmark/winflexbison)，
   把 `flex.exe` 和 `bison.exe` 放到已知目录（如
   `C:\dev\win_flex_bison\`）
3. 显式给出 Flex/Bison 路径进行配置：
   ```bash
   cmake -B build -DFLEX_EXE="C:/dev/win_flex_bison/flex.exe" \
                  -DBISON_EXE="C:/dev/win_flex_bison/bison.exe"
   ```
4. 构建脚本使用 Python3，且在默认 `NLANG_BUILD_DOCS=ON` 下文档站
   （mkdocs）也使用它（`pip install -r tools/docs-requirements.txt`，
   解释器可用 `-DNLANG_DOCS_PYTHON=<path>` 指定；关闭文档用
   `-DNLANG_BUILD_DOCS=OFF`）。`-DPYTHON3_EXECUTABLE=<path>` 覆盖
   构建脚本使用的解释器。

## 构建

```bash
cmake -B build
cmake --build build
```

### 构建选项

| 选项 | 默认值 | 说明 |
|--------|---------|-------------|
| `-DNLANG_ENABLE_LLVM` | OFF | 启用 LLVM 代码生成后端 |
| `-DNLANG_BUILD_IDE` | OFF | 构建 Qt5 IDE（nide） |
| `-DNLANG_BUILD_TESTS` | OFF | 构建单元测试 |
| `-DNLANG_BUILD_DOCS` | ON | 构建 mkdocs 文档站 |
| `-DNLANG_DOCS_PYTHON` | `python` | 运行 mkdocs 的解释器（见 `tools/docs-requirements.txt`） |
| `-DFLEX_EXE` | 自动 | flex 可执行文件路径 |
| `-DBISON_EXE` | 自动 | bison 可执行文件路径 |
| `-DPYTHON3_EXECUTABLE` | 自动 | 构建脚本使用的 Python3 路径 |

### Windows 上已知的 Flex/Bison 问题

- **`unistd.h` 包含**：Flex 2.6 的 `%option nounistd` 并不能阻止
  生成的头文件包含 `<unistd.h>`。我们用 CMakeLists.txt 中的
  `YY_NO_UNISTD_H` 编译定义处理。
- **`INT8_MIN` 宏重定义**：Flex 在非 C99 回退分支中生成
  `INT8_MIN/MAX` 宏，而 MSVC 的 `<stdint.h>` 也定义它们，导致
  C4005 警告。我们在 `nlang.l` 中用 `%top{ #include <stdint.h> }`
  处理——让 `<stdint.h>` 先于 flex 的定义被包含，`#ifndef` 守卫
  便会跳过冲突的宏。

## 命令行工具

```text
ncc <source.n> [-o out.nmod] [-I <dir>...]        编译并执行
ncc build <source.n> [-o out.nmod] [-I <dir>...]  仅编译
ncc -p <project.nproj> [-o out.nmod] [-I ...]     编译并执行一个项目
ncc build -p <project.nproj> [-o out.nmod]        编译一个项目
ncc run <module.nmod>                             仅执行
```

### 调试

```text
ndb <module.nmod>   调试已编译的模块（像 gdb `start` 一样在首条
                    语句处初始停驻）
```

命令：`b <file.n:LINE | LINE | funcName>` 设断点（裸 `LINE` 在
当前帧的文件中解析），`i b` 列出断点，`d <id>` 删除，`c` 继续，
`s`/`n`/`f` 单步进入/跳过/跳出，`bt` 回溯，`frame <n>` 选择帧，
`info locals` 列出局部变量，`p <name>` 打印一个局部变量，
`l [line]` 源码窗口，`x` 当前帧的反汇编，`catch on|off` 抛异常时
中断（默认 off），`q` 退出——stdin EOF 等同 `q`。程序结束时 ndb
打印 `Program exited with code N.` 并以同一退出码退出。

面向嵌入场景，`ndb --machine <module.nmod>` 在 stdin/stdout 上以
制表符分隔的行协议暴露同一会话——nide 调试器即构建于其上。引擎侧
分层见 `docs/zh/vm-architecture/debugging.md`。

所有命令行工具用 `--version` 报告版本（如 `ncc (NLang) <version>`）。
IDE 在 帮助 → 关于 中显示，文档站在页脚显示。

多源项目由 `.nproj` XML 文件描述（见 `examples/hello_project/`）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project" namespace="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` 是输出模块名（默认取文件名主干），`outputDir` 可选地重定向
`.nmod`（相对项目文件），`File` 路径相对项目文件所在目录。

## 模块与导入

跨文件引用是显式的：`import` 声明一个文件可以调用哪些模块，编译器
拒绝其余一切（同目录文件是唯一例外——它们隐式互相可见）。三种
import 来源共用一种语法：

| 来源 | 模块路径 | 示例 |
|---|---|---|
| 项目文件 | 相对 `.nproj` 的点分路径 | `utils/helper.n` → `import utils.helper;` |
| 外部 `.nmod` | 文件名主干（单段） | `lib.nmod` → `import lib;` |
| 内建命名空间 | `io` / `math` / `fs` | `import io;` |

| 引用目标 | 需要 import？ | 调用形式 |
|---|---|---|
| 同一文件 | 否 | 裸名 |
| 同目录其他文件 | 否（隐式） | 裸名或限定名 |
| 跨目录、同项目 | **是** | 仅限定名（`utils.helper.f()`） |
| 外部 `.nmod` | **是** | 仅限定名（`lib.f()`） |
| 内建 `io`/`math`/`fs` | **是** | 限定名（`io.print`） |

此矩阵覆盖根级函数。跨目录共享的命名空间成员是 v1 的唯一例外——
任何形式都无法从另一目录访问（见「声明」章的 Import Declaration）。

通配 import 是递归前缀匹配：`import utils.*;` 可达 `utils/` 及其
所有嵌套子目录（`utils.helper`、`utils.sub.x`）。调用仍使用完整
路径——没有 `from m import *` 形式。重复与重叠的 import 幂等。

未导入的引用以精确诊断失败，例如
`Module 'utils.helper' is not imported. Add 'import utils.helper;' (or 'import utils.*;') at the top of this file.`
完整语义——解析顺序、保留路径段、单文件模式——见「声明」章
（`docs/zh/language-spec/declarations.md`，Import Declaration）。

编译模块使用带版本的二进制格式，当前为 v1.11。加载器强制兼容性
下限：下限提升后，较旧的 `.nmod` 会因过期被拒绝，必须用匹配的
`ncc` 重新编译。格式历史（每个版本新增或变更了什么）见
[CHANGELOG.md](CHANGELOG.md)。

## 标准库（Phase 11）

`math`、`io` 和 `fs` 是内建命名空间——保留名，限定调用前需要显式
`import`。字符串自带内建方法：

```n
import io;
import math;
import fs;

int main() {
    io.print(math.sqrt(2.0));                   // 1.41421
    string s = "hello world".substring(0, 5);   // "hello"（字节偏移）
    List<string> words = "a,b,c".split(",");    // 3 个元素
    fs.makeDirs("out");
    io.writeFile(fs.join("out", "greet.txt"), s);
    if (!fs.exists("out/greet.txt")) return 1;
    return words.length();
}
```

完整细节——参数类型政策、异常映射、字节语义、确定性
PRNG——见标准库章（`docs/zh/language-spec/standard-library.md`）；
示例的可运行副本在 `examples/stdlib_*.n`。

初次接触 NLang？`docs/zh/getting-started/` 章节按主题逐页讲解
语言、命令行与 IDE（也可从 nide 帮助菜单到达）；
`examples/README.md` 索引了全部可运行示例。

## IDE（nide）

```bash
cmake -B build-ide -DNLANG_BUILD_IDE=ON \
    -DCMAKE_PREFIX_PATH=<path-to-qt5.15>
cmake --build build-ide --config Release
```

`build-ide/src/tools/nide/Release/` 下的构建输出本身就是可运行的
布局：Qt 运行时 DLL、`platforms/` 插件目录，以及 `ncc`/`nvm`/`ndb`
工具都被复制到 `nide.exe` 旁边（IDE 从自身目录调用它们），因此该
目录可以原样复制到别处使用。
界面语言默认跟随系统区域设置，可在 工具 → 选项 中固定为中文或
英文（重启后生效）；未翻译的字符串回退到其原文。

通过 文件 → 新建文件 创建的文件加入解决方案树中选中的项目
（或解决方案的唯一项目）；文件通过 F2 或树/页签右键菜单原地
重命名（有未保存修改的打开编辑器会先保存到旧路径），分割器布局
跨会话持久化，文件 → 最近打开 记住最近的解决方案、项目与文件。

通过 文件 → 打开 打开的独立 `.n` 文件（无需项目）出现在
「独立文件」树分组中，可直接构建与运行：模块落在
`%TEMP%\nlang-nide\` 下，源文件变化后「运行」会自动重建它。
帮助菜单在 IDE 内嵌查看器中显示随包分发的文档站
（`docs/site`）。

nide 还自带调试器（底层驱动 `ndb --machine`；完整演练见
`docs/zh/getting-started/debugging.md`）。F5 启动调试会话——程序
运行到首个断点或运行到底——Shift+F5 随时停止：停止是硬终止，
因此死循环或卡住的原生调用不会阻塞 IDE。断点用 F9 或行号槽点击
切换（行号槽圆点初始为空心，实会话确认该行存在于已编译模块后
变实心），跨会话持久化并跟随文件重命名。停驻期间，输出区的
「调试」页显示调用栈（点击帧跳转并刷新局部变量）、该帧的局部
变量，以及在每个抛出点中断的「抛异常时中断」开关。

| 动作 | 快捷键 |
|------|--------|
| 启动调试 / 继续 | F5 |
| 运行（不调试） | Ctrl+F5 |
| 停止调试 | Shift+F5 |
| 切换断点 | F9 |
| 单步跳过 | F10 |
| 单步进入 | F11 |
| 单步跳出 | Shift+F11 |

`ctest -C Release -R nide_deploy_check` 验证自包含性：把布局复制
到临时目录，用 `qt.conf` 把 Qt 的搜索路径钉在那里，并从那里运行
IDE 测试套件。

## 文档站点

双语用户手册（`docs/zh/` 与 `docs/en/`，每树一份 mkdocs 配置，
共同继承 `mkdocs.base.yml`）由 CMake 目标 `nlang_docs` 渲染成静态
站点（默认开启，`-DNLANG_BUILD_DOCS=OFF` 跳过），合并到
`<build>/docs/site/{zh,en}` 并置于语言选择落地页
（`docs/site/index.html`）之后；IDE 的帮助菜单按语言设置打开对应
树中的页面。工具链版本钉在 `tools/docs-requirements.txt`
（`pip install -r tools/docs-requirements.txt`，再用
`-DNLANG_DOCS_PYTHON=<解释器>` 配置）。

管线位于 `tools/nlang-docs/`，是自包含的包、不安装——通过
`PYTHONPATH` 直接从源码树运行（Git Bash 的 env 前缀语法；在
PowerShell/cmd 上使用 CMake 配方自己用的
`cmake -E env PYTHONPATH=... <python> ...` 惯用法）：

```bash
# 迭代预览（普通 mkdocs 热重载服务器；任选一树）
PYTHONPATH=tools/nlang-docs/src python -m nlang_docs serve --config mkdocs.zh.yml

# CMake 目标对每棵树做的事（mkdocs.zh.yml 与 mkdocs.en.yml 都要）：
# mkdocs build --strict、离线搜索内联与片段审计——站点审计延迟到
# 两树都建成后（它们互相链接），`check` 再对每树各跑一次
PYTHONPATH=tools/nlang-docs/src python -m nlang_docs build \
    --config mkdocs.zh.yml --site-dir build/docs/site/zh \
    --defer-site-audit \
    --ncc build/src/tools/ncc/Release/ncc.exe \
    --nvm build/src/tools/nvm/Release/nvm.exe

# 审计已生成站点的某一树（打包验证也复用）
PYTHONPATH=tools/nlang-docs/src python -m nlang_docs check \
    --site-dir build/docs/site/zh --config mkdocs.zh.yml

# 独立审计某一页的 ```nlang 片段（编译 + 运行 + 退出码）
PYTHONPATH=tools/nlang-docs/src python -m nlang_docs snippets \
    --doc docs/zh/getting-started/first-program.md --ncc <ncc> --nvm <nvm>
```

`build` 串联四个阶段：mkdocs 构建、离线搜索内联、站点审计，以及
——当已知 `--ncc`/`--nvm`（CMake 目标总是传入）时——片段审计：
编译并运行入门指南中的每个 ```nlang 程序，与片段承诺的进程退出码
比对，因此破坏了文档示例的语言变更会让文档构建失败。

站点必须从 `file://` 完全离线渲染：URL 保持扁平
（`use_directory_urls: false`）、禁用网络字体、不从任何 CDN 加载，
且构建把搜索索引内联进 `search/search_index.js` 本身
（mkdocs-material 的 offline 插件也能做，但它会注入来自 unpkg 的
CDN polyfill，因此管线手工实现内联——见
`tools/nlang-docs/src/nlang_docs/offline_search.py`）。`check`
审计强制这一形态：内部链接必须解析到已存在的 `.html` 文件、
`#fragment` 必须存在、目录形式链接被拒绝、已构建页面集合必须等于
`--config` 传入的每树 `nav`（缺失与不可达都会失败），且任何
`script[src]`/`link[href]` 不得引用 http(s)——站点必须在完全没有
网络时加载。单元测试：`pytest tools/nlang-docs/tests`（也以
`nlang_docs_pytest` 接入 ctest）。

## 打包（Windows）

发布包从 IDE 构建树用 CPack 产出（它包含全部工具；纯工具树无法
分发 `nide`）：

```bash
cmake -B build-ide -DNLANG_BUILD_IDE=ON \
    -DFLEX_EXE=<flex> -DBISON_EXE=<bison> \
    -DCMAKE_PREFIX_PATH=<path-to-qt5.15> \
    [-DNLANG_NSIS_MAKENSIS=C:/path/to/makensis.exe]
cmake --build build-ide --config Release
cd build-ide && cpack -C Release -B ../release
```

这会产出 `release/NLang-<version>-win64.zip`（便携版）和
`release/NLang-<version>-win64.exe`（NSIS 安装程序；需要 NSIS
3.03+——在 `PATH` 上或经 `-DNLANG_NSIS_MAKENSIS` 传入）。两者包含
相同布局：`bin/`（`nide`、`ncc`、`nvm`、`ndisasm`、`ndb` 与 Qt
运行时），外加 `examples/`、生成的文档站（`docs/site/`）、
`LICENSE`、`CHANGELOG.md` 与 `README.md`。安装程序默认装到
`C:\Program Files\NLang` 并为 IDE 添加开始菜单快捷方式。

注意：

- 安装程序默认装到标准用户不可写的 `C:\Program Files\NLang`。在
  IDE 中打开 `examples/` 前先复制到可写目录——构建会在项目文件旁
  写 `.nmod`。
- 可执行文件动态链接 MSVC 运行时；目标机器需要
  [Visual Studio 的 VC++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  （装有 Visual Studio 2022 的机器已具备）。
- `python tests/packaging/verify_package.py` 校验构建出的包：解压
  zip、断言布局、用公开文本守卫扫描随包散文（不得出现对私有前代
  代码库的引用——与 `nlang_docs_pytest` 门同一模式源）、用
  `nlang_docs check` 管线审计打包的文档站，并用包内工具链编译运行
  `examples/hello.n` 做冒烟测试。

## 项目结构

```
include/nlang/runtime/     - 运行时公共头文件
include/nlang/compiler/    - 编译器公共头文件
include/nlang/vm/          - VM 公共头文件（.nmod 格式常量）
src/runtime/               - 运行时实现
src/compiler/              - 编译器实现（文法、生成代码、构建器）
src/vm/                    - VM 后端实现
src/tools/ncc/             - 命令行编译器
src/tools/nvm/             - VM 运行器
src/tools/ndisasm/         - 字节码反汇编器
src/tools/ndb/             - 调试器
src/tools/nide/            - Qt5 IDE
src/3rdparty/tinyxml2/     - 内置的 tinyxml2 10.1.0（.nproj 解析）
```

## 许可证

MIT
