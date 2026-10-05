## 基本原则
- **独立验证后再同意**：当用户提出一个观点或诊断时，必须基于代码和日志独立验证其正确性，确认无误后才可同意。如果验证发现观点有误，应指出问题而非迎合。
- 提交前必须先经过用户确认。
- 程序要符合SOLID设计原则。
- 请使用测试驱动开发方式。
- 在实现相同功能的前提下，尽可能保证实现的简洁性和易维护性。
- 代码修改前，要从总体上理解问题本质，然后再修改，不要只改表面遇到的报错。
- 防御式编程侧重于输入数据缺漏处理，不做过度安全防护。
- **疑难问题联网搜索**：遇到疑难问题（如工具链兼容性、平台特定 bug、API 变更等），应联网搜索并参考已知最佳解决方案，而非仅凭经验猜测。优先查阅官方 issue tracker 和权威社区。
- 尽量保持代码优雅，不要加入太多难以理解和维护的hack。
- 各种名称在保持简洁性的同时，尽量让名字一目了然，不易混淆。
- 解决疑难问题后，遇到复杂的程序逻辑要添加注释，让后续维护者能理解设计意图。
- 计划和注释等，尽量用规范语言描述，避免使用小众口语化表达，增加可读性。
- 当前还没有大规模使用，不需要向下兼容，用户手册中也不用提及不同版本内容差异等内容。
- 如果为达成某个功能，导致实现复杂且容易出错，要反思是否有更优雅的解决方案，必要时可以联网搜索类似问题的解决思路。
- 尽量用标准术语，不要用自创的缩略语，以便阅读和复用，避免歧义。

## 循环完善流程
- "审核-验证-完善"往复的多轮循环，直到**审核后没有发现新的问题，而不是发现问题后修改解决了**，但不应为逃避问题而减少审查。
- 若某轮审查**只发现低级别（Low/Info）问题**，修正后测试全部通过，即可进入下一阶段，无需再做完整的重新审核；中高级别（Medium及以上）问题仍须循环到零新发现。
- 解决问题时，要从全局考虑问题，不要只解决表面的现象，否则可能改错了。
- 发现的每个问题，都要经过 superpowers 全面分析审核后，再解决。

## 项目背景
- NLang 是面向嵌入与自动化场景的静态类型脚本语言 + AI 友好特性试验台，自带编译器、字节码 VM、调试器与 IDE，作为教学/研究开源项目（公开定位 2026-09-12 定稿，见 README）。
- **公开文本零前代痕迹（硬门禁）**：公开仓库与发布包不得出现前代引擎相关的任何描述或标识符，禁用模式清单以 `tools/nlang-docs/src/nlang_docs/public_text.py` 为单一模式源（ctest `nlang_docs_pytest` 扫描 tracked 文件 + `verify_package.py` 扫描发布包；守卫自检样本在两个守卫文件内，测试已自指豁免）。历史语义追溯只走本地记录，不写进公开文本。
- 分支模型：`master` 为稳定发布分支，功能开发在 `dev` 分支进行，完成后合入 `master`。
- 各阶段实施计划与设计稿位于 `docs/dev/`（随分支跟踪；2026-09-30 起自 `temp/` 迁入，库管理重构线的阶段 3～7 文档都在此，`docs/dev/README.md` 为索引）。

## 开发环境
- C++17，CMake 3.16+，Flex 2.6+，Bison 3.0+。
- IDE 构建需要 Qt5（Core/Xml/Widgets 模块）。
- LLVM 为可选依赖（`-DNLANG_ENABLE_LLVM=ON`），默认使用自定义字节码 VM 后端。
- 构建命令：`cmake -B build -DNLANG_BUILD_IDE=ON -DNLANG_BUILD_TESTS=ON -DFLEX_EXE=<winflexbison>/flex.exe -DBISON_EXE=<winflexbison>/win_bison.exe && cmake --build build --config Release`（Qt5 需另加 `-DCMAKE_PREFIX_PATH=<Qt>`）。注意：`NLANG_BUILD_TESTS` 默认 OFF——漏配则 ctest 报 "No tests were found" 但 exit=0 假绿，必须核对测试计数。
- Python 脚本使用 conda 环境名 `py313`。
- 项目要支持离线构建和部署。

## 编码规范
- magic number 定义为常量（如 `DEFAULT_*`）。
- 命名具体化：避免 `cache`/`result`/`data` 等泛指。
- 优先使用 STL 标准库，避免引入 boost 等外部依赖。
- 智能指针优先用 `std::unique_ptr`/`std::shared_ptr`；遗留 `EnNew`/`EnDelete` 宏已移除，直接用 `new`/智能指针。后续目标：全面 RAII 化（智能指针替换裸 `new` 与自定义 `UniquePtr` 容器）。
  - 拥有指针：`m_up*`（unique_ptr）或 `m_sp*`（shared_ptr）；`m_p*` 仅用于非拥有裸指针
- 拷贝禁用用 `= delete`，不用旧 `CopyDisabled` 基类。
- 文件路径操作用 `std::filesystem`（C++17）。
- 所有代码统一在 `nlang::` 命名空间下，不保留 `en::` 引擎命名空间。
- 注释风格：文件头用 `/*--- ... ---*/`，简短行注释，不写冗长 Doxygen 文档。

## 架构要点

### Visitor 模式自动生成
- `SyntaxNodeVisitor.h` 通过 `SYNTAX_NODE_DECL` 宏自动生成 Visit 方法。
- 新节点只需添加到 `SyntaxNodeConsts.h` 的 `COMPILE_ONLY_NODE_DECL` 宏即可，无需手动编辑 Visitor。

### 局部变量声明分解模式
- `int x = 1` 在名称解析阶段分解为：1) 变量注册到父 Paragraph 的局部变量表；2) 若有初始值，自动创建 AssignStmt。
- `SnParagraph::FindField` 先查局部变量表，再查父类。

### VM 结果指针模型
- NLang VM 使用结果指针模型（非操作数栈）。每条指令接收 `void* pResult`，由调用方决定结果写入位置。
- `EmitExpression` 接收 `resultOffset` 参数，指定结果写入的 local slot。

### X-macro 缩进
- X-macro 宏体生成的 case 语句必须与 switch 体中的 `default:` 对齐。

## 模块边界
- `nlang_runtime` — 运行时库，无外部依赖。
- `nlang_compiler` — 编译器前端，依赖 runtime，可选依赖 LLVM。
- `nlang_vm` — 字节码 VM 后端，依赖 runtime。
- `ncc` — 命令行编译器，依赖 compiler + runtime + vm。
- `nvm` — VM 执行器，依赖 vm。
- `ndisasm` — 字节码反汇编器，依赖 vm。
- `nide` — Qt5 IDE，依赖 compiler + runtime + vm + Qt5。
- 公共头文件在 `include/nlang/{runtime,compiler,vm}/`，内部实现在 `src/`。

## 新增或变更功能
- 新增或变更功能时，要考虑功能的适用性，避免添加无用功能。
- 新增或变更功能时，不应影响原有其他功能。
- 接口变更要同步更新 `include/` 下的公共头文件和 `README.md`。
- 新增或修改用户可见功能时，同步提升版本号（`VERSION` 次版本 +1；修复补丁 +1）并在 `CHANGELOG.md` 对应版本节记录；发布时补日期 + tag（详见 CONTRIBUTING）。
- 根部文档中英成对维护：`README.md`↔`README.zh-CN.md`、`CHANGELOG.md`↔`CHANGELOG.zh-CN.md`（顶部互链；中文版 docs 链接指向 `docs/user_manual/zh/` 树；随包分发）——改英文版必须同步中文版。
- 编写用户手册时，应参考`docs/user-manual-style-guide.md`。

## 目录结构规范
- 公共头文件统一放 `include/nlang/`，按 `runtime`/`compiler`/`vm` 分子目录。
- 内部实现文件放 `src/` 下对应模块目录。
- 文档统一放 `docs/`（2026-08-21 起：原规则写 `doc/`，但现存文档一直全在 `docs/`，按既成事实修正，`doc/ci_design.md` 已迁入）。用户手册双树在 `docs/user_manual/{zh,en}`（2026-09-30 自 `docs/{zh,en}` 迁入）；开发向设计文档在 `docs/dev/`。
- 示例 NLang 程序放 `examples/`。
- Flex/Bison 文法文件放 `src/compiler/grammar/`。
- 临时文件和脚本，请放到 `temp/`。
- 手册双树的编辑政策与术语表见 docs/user-manual-style-guide.md（紧凑空格与缩略语共现两项守卫在 docs 测试套件内强制）。

## 测试
- 单元测试覆盖率应该大于 90%。
- 测试要包含真实测试，不要都用 mock。例如，编译器测试要真实编译 `.n` 文件，VM 测试要真实执行字节码。
- 每个测试用例，超时时间设的短一些（例如最长 3min）。
- 涉及 IDE 的修改，必须包括浏览器交互验证（使用 Playwright 等）。
- e2e 测试脚本用 Python（conda 环境 `py313`），位于 `tests/e2e/`。
- e2e 测试清单在 `tests/e2e/manifest.txt`（格式：`test_name expected_exit_code`）。

## 提交
- 提交之前，请使用 superpowers 插件执行"循环完善流程"。其中，验证指完整的测试。
- 提交信息用英文，遵循 Conventional Commits 风格。

## 术语
- NLang — 本脚本语言
- NComp — 编译器前端（词法/语法/语义分析、AST）
- Runtime — 运行时（模块加载、执行引擎）
- VM — 自定义字节码虚拟机
- ncc — NLang Command-line Compiler
- nvm — NLang VM Runner
- ndisasm — NLang Bytecode Disassembler
- nide — NLang IDE
- .n — NLang 源文件扩展名
- .nmod — 编译后的 NLang 模块文件扩展名
