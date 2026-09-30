# Phase 4b/4c/4d 设计（混合库覆盖 · kStdLibTable 删除 · 变更感知重编译）

> 状态：设计稿（docs/dev/，随分支跟踪；2026-09-30 自 temp/ 迁入）。分支：dev（worktree `E:\cases\nlang\dev`）。
> 上游设计：`docs/dev/phase4_design.md`（4a 已提交 `ab4546a`）。
> 本文只覆盖剩余三项；每条决策附「已验证证据」与「备选方案 + 否决理由」。

## 0. 前置事实校正与裁决记录

> **裁决（2026-09-28，用户）**：4b 单独摘出先提交；4c 批准「三套 intrinsic
> 实现一并退役」；4d 选 **方案 A**（库源依赖清单进 `.nmod`）。

- 本工作区在本次会话开始前就带有**未提交的他人/前序会话改动**（Phase 4b-2：
  `nlang.y` 的 `QualifiedType`/`CollectQualifiedSegments`、`ModuleRegistry`、
  `ExprResolverTypes`、`Register.cpp`、`test_library_source.cpp` 的
  types/inherit/iface/enum_method 用例）。会话初始 `git status` 因
  content filters 报 "working-tree cleanliness is unknown"，未采集，
  导致先前汇报把「在制」误判为「已落地」。
- 结论：任何后续提交都必须先把「我的改动」与「4b-2 在制改动」分离，
  提交归属与顺序由用户裁决。
- 复核（2026-09-28）：`-j 8` 并行跑全量 ctest 时 `mainwindow_tests`、
  `nide_deploy_check` 失败；单独串行复跑均通过（39.11s / 38.79s）。
  判定为共享临时态/QSettings 争用导致的**并行假失败**，非回归。
  验证协议相应调整：全量 ctest 串行，或至少把这两个隔离重跑。

## 1. Phase 4b — 混合库 in-process 覆盖

### 目标
把「同一个 `.n` 内：`native` 声明 + 普通 NLang 函数体调用该 native」从
shell 级 e2e（`check_nvm_native.py` / `use_mylib.n`）上移为 in-process
真实测试：真编译（`ModuleBuilder`）+ 真加载 DLL + 真执行（`VmExecutor`）。

### 决策 1：新增独立命名空间 fixture（mixlib），不复用 mylib
已验证证据（试错过程）：
1. 复用 mylib 时，两场景把**同基名** `nlang_mylib.dll` 各自复制到自己的包目录；
   第二个执行器崩溃 `0xc0000409`（fastfail）。Windows 的映像加载按**基名**
   去重，第二个 `LoadLibrary` 复用第一个映像，第一个 `VmExecutor` 析构
   `FreeLibrary` 后函数指针失效；
2. `ModuleManager::Create`（`src/runtime/Module.cpp:56`）进程级唯一、
   **无卸载 API**，同名模块第二次构建直接 `EEXIST`；
3. 两场景若写同名 `prog.nmod` 而加载路径与 `m_sOutputModule` 不一致，
   会在 `ModuleLoader::Load` 处崩溃（本次即命中过）。

备选：
- *每场景独立子进程（nvm 跑）*：与既有 e2e 重复，不新增 in-process 覆盖，否决；
- *一个进程内只保留一个 native 场景*：丢覆盖率，否决。

结论：`tests/fixtures/native/mixlib/{mixlib.n, mixlib_native.cpp}`，
native `dbl` + NLang `quad = dbl(dbl(x))`；CMake 目标 `nlang_mixlib`，
DLL 基名经 `$<TARGET_FILE_NAME:nlang_mixlib>` 注入（跨平台）。

### 决策 2：场景目录名即模块名，包目录彼此隔离
沿用 `test_library_source.cpp` 的 `scenarioDir` 惯例；`buildAndRun` 用
`pkg.filename()` 同时作为 `m_sOutputModule` 与 `.nmod` 文件名（消除名称
/路径不一致）；`packageDir()` 启动 `remove_all` 清临时树（陈旧 `.nmod`
会伪装成构建失败）。已在代码注释里记录 Windows 基名这一非显然约束。

### 测试面（先红后绿）
`mixlib.quad(3)==12`（NLang 体两次调 native）、`mixlib.dbl(5)==10`
（限定名直调 native）、`io.print` 走宿主 IO 捕获 `"mixed ok\n"`。

### 状态
已提交 `da5e4a8 test(vm): cover mixed native + NLang libraries in process`。
摘出过程做了**独立性验证**（而不是假设）：把 21 个 4b-2 在制文件逐文件
sha256 备份到 `temp/4b2_backup/` 后回退到 HEAD，「HEAD + 仅 4b」Release 全建
0 error、串行 ctest 63/63 通过（`ncc_thirdparty_run` 覆盖 `addTwice`、
`thirdparty_tests` 覆盖 mixlib）；随后原样恢复并在合树复跑 63/63。
结论：4b 不依赖 4b-2 的限定类型改动（后者只涉及类型位置，函数路径 4a 已具备）。
并行 ctest 下 `mainwindow_tests`/`nide_deploy_check` 争用临时目录与
NLANG_TEST_ROOT 导致假失败，验证协议固定为**串行**。

## 2. Phase 4c — 删除 kStdLibTable 及其不可达实现

### 已验证的现状清单
| 对象 | 位置 | 引用状况 |
|---|---|---|
| `kStdLibTable` / `StdLibEntry` / `FindStdLibFunction` | `include/nlang/vm/StdLib.h:62-198, 288` | `src/` 零调用（4a 删除了最后引用）；仅自身 `static_assert` + `tests/check_stdlib_generation.py:43-113` + `tests/test_vm/test_stdlib.cpp:279` |
| `IsStdLibNamespaceName`（保留名 io/math/fs） | `StdLib.h:54` | **仍在用**：`ModuleRegistry.cpp:81` 保留段守卫 |
| math/io/fs intrinsic 分派臂 | `src/vm/IntrinsicsMath.cpp`(223) / `IntrinsicsIo.cpp`(145) / `IntrinsicsFs.cpp`(208)，由 `VmExecutorIntrinsics.cpp:421-427` 调用 | **发射器为零** → 4a 后运行时不可达（576 行死码） |
| id 常量与连续块断言 | `include/nlang/vm/CompiledModule.h:183-274`（`kMathIntrinsicFirst=70/Count=25`、`kIoIntrinsicFirst=110/5`、`kFileSystemIntrinsicFirst=120/8`） | 仅被上述家族与 `test_stdlib.cpp:279` 引用 |
| string 方法 `INTR_String_*`(95-106) | 同上文件 | **保留**：接收者分派内建方法仍走 `OP_CallIntrinsic` |

### 决策：不可达家族一并退役，而不是留着（已批准）
理由：留着等于让 io/math/fs 存在两套实现（DLL 与 VM 内建），二者会漂移，
直接违背「统一机制」与 §基本原则的简洁要求；且项目明确不需要向下兼容。

执行顺序（TDD，每步全量回归）：
1. 先加守卫测试（红灯可验）：
   - 「`kStdLibTable` 家族在 `src/`+`include/` 无引用」的源码级 grep 守卫；
   - 保留名守卫：项目里声明 `io`/`math`/`fs` 模块目录被拒（现由
     `FindReservedSegment` 保证，测试固化）；
   - `stdlib/*.n` 与三个 DLL 的**行为**用例已在 `test_stdlib.cpp`/e2e 中，
     确认它们不再依赖表。
2. 删除 `StdLib.h` 的表/条目/查找器与其 `static_assert`；保留名抽为独立
   常量（`kReservedLibraryNames` + `IsReservedLibraryName()`），迁移
   `ModuleRegistry.cpp:81` 的引用。
3. 删除三个 `Intrinsics*.cpp` + `VmExecutorIntrinsics.cpp:421-427` 三个
   调用臂 + `CompiledModule.h` 的 math/io/fs id 块与断言；
   `src/vm/CMakeLists.txt` 去文件；`tests/test_stdlib.cpp:279` 的计数断言
   改为按 `stdlib/*.n` 声明数（表没了就没基准了）。
4. 移除 `tests/check_stdlib_generation.py` 与 ctest `stdlib_generation`
   （`tests/CMakeLists.txt:875-878`）。
5. 同步文档：重写 `docs/{en,zh}/vm-architecture/standard-library-intrinsics.md`
   ——该页现在整篇描述已删除的签名表路径，须收敛为「string 方法 +
   其余家族」，并指向库机制页；更新
   `docs/{en,zh}/language-spec/standard-library.md` 的「过渡期/未来方向」段
   （kStdLibTable 不再是过渡角色）；`docs/roadmap.md` 增补阶段成果摘要。

否决项：*保留一个「stdlib 一致性」新校验脚本比对 .n 与 DLL 导出*——真实
调用已由 `test_stdlib.cpp` + `check_nvm_native.py` 端到端覆盖，再加一层
静态对拍属重复建设。

## 3. Phase 4d — 变更感知的自动重编译（需用户裁决）

### 问题
nide standalone 过期检测（`MainWindowBuildRun.cpp:191`）只比较主源 `.n`
与 `.nmod` 的 mtime；4a 后库 `.n` 也参与编译，改了库源不会触发重编。

### 方案 A：参与库源清单（路径 + mtime）序列化进 `.nmod`（**已选定**）
- 优点：单一事实源，发现规则只活在编译器；nvm/ndb/未来包校验可复用同一
  清单（ndb 定位库源本来就需要 `sourceFile`）；离线可读。
- 代价：动模块格式（v1.13）；`ModuleSaver/ModuleLoader/TypeDesc` 序列化
  面积增加；旧 `.nmod` 需重编（项目允许）。

### 方案 B：`ncc deps` 查询模式，输出内联库源列表
- 优点：不动格式，改动面最小。
- 代价：nide 每次判废要跑两次 ncc；结果不落盘，ndb/包校验无法复用；
  「查询」与「真实构建」若发现逻辑分叉仍会漂移（除非严格复用同一函数）。

### 建议
倾向 **A**：`.nmod` 本来就是「这次编译用了什么」的权威记录，库源依赖天然
属于它；且 4d 后续 ndb 库源调试也要用同一份信息。但这是格式级决定，
请用户选定后再实施。

### 4d 其余项（无争议，顺序靠后）
- `runNccBuild` 由同步 `waitForFinished` 改异步（`QProcess::finished` +
  状态门），否则自动触发构建会卡 UI；
- ndb：验证断点/步进/`l` 能解析库 `.n`（`sourceFile` 绝对路径已在），
  缺源时给可读提示；
- 文档（中英）：混合库编写指南、库源改动重编行为、查看库源；翻译 lrelease；
- 按 AGENT.md 提升 VERSION 次版本 + 中英 CHANGELOG 成对记录。

## 4. 提交切分（4b 已完成）
- ~~4b~~ → `da5e4a8`，仅测试面（`tests/CMakeLists.txt` + `test_thirdparty.cpp`
  + `mixlib` fixture + `mylib` 混合体用例）；
- 4c：提交 1 = 代码删除（表 + 三套 intrinsic + id 块 + 生成器/ctest + 守卫测试），
  提交 2 = 文档（`standard-library-intrinsics.md` 中英重写、
  `standard-library.md` 过渡段、`roadmap.md`、`library-mechanism.md` 中英 + nav）；
- 4d：提交 1 = `.nmod` v1.13 依赖清单（编译器记录 + Saver/Loader 序列化），
  提交 2 = nide 过期检测改读清单 + `runNccBuild` 异步化 + ndb 库源验证，
  提交 3 = 文档 + VERSION/CHANGELOG；
- 4b-2 在制改动由用户自行提交，我的提交不与其文件重叠；`temp/`、`build-dev/` 不提交。
