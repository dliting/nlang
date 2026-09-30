# Phase 4 设计：混合库（native + NLang）与统一库机制

> 状态：设计稿（docs/dev/，随分支跟踪；2026-09-30 自 temp/ 迁入）。分支：dev（worktree `E:\cases\nlang\dev`）。
> 参考：Python（源文件 + C 扩展同包、源改动按 mtime 重编）、Java/C#（托管方法 + JNI/P/Invoke 混合）、C 头文件 + 链接库模型。

## 1. 目标

1. **混合库**：一个第三方库可同时包含 `native` 函数（外部 DLL 实现，只暴露签名）与普通 NLang 函数（提供实现）。普通 NLang 函数可调用同库 native 函数，反之通过 ABI 回调。
2. **源码可查看、可修改、自动重编译**：用户能在 nide 中打开/跳转到库源 `.n`；修改库 `.n` 后，依赖它的模块自动重新编译。
3. **统一机制，淘汰 `kStdLibTable`**：标准库与第三方库走完全相同的「源内联 + native DLL」路径；删除 VM 内硬编码的标准库表与其编译期校验。
4. **配套**：nide（库源打开/跳转、重编译触发）、ndb（进入库源码的断点/步进）、中英文档；为将来 LSP 复用同一套语义分析。

## 2. 现状（探查确认的事实）

- import 两条路径（`ModuleBuilderImports.cpp`）：
  - **`.n` 源库**：`DiscoverLibrarySources()` 对单段 import 在搜索路径找 `<name>.n`，只调 `LoadLibrarySource()` → `SymbolIndex::LoadFileOnce()`，**仅索引签名，不解析函数体**。
  - **`.nmod` 预编译**：`LoadExternalModule()` 找 `<name>.nmod`，建 stub、注册 EXTERNAL 模块。
- native 三套机制：
  - `kStdLibTable`（`StdLib.h`）：`ns,name → intrinsicId`。运行时 math/io/fs **已不再经此表**（codegen 对库调用走 `OP_CallFunc` + native stub）；该表现仅用于编译期校验 `FindStdLibFunction()`（`ExprResolverStdLib.cpp:209`）。
  - 进程内 host native（`RegisterNative`，裸名，测试用）。
  - 动态 DLL（`NativeLibraryLoader`）：按需加载 `nlang_<ns>.dll`，运行 `nlang_native_init`，注册到执行器 `m_natives["ns.name"]`。
- 标准库 `nlang_math/io/fs.dll` 已构建并部署到各工具目录；`stdlib/*.n` 由 `check_stdlib_generation.py` 从 `StdLib.h` 自动生成（文件头 DO NOT EDIT），ctest `stdlib_generation` 校验一致性。
- 代码生成：`EmitStdLibCall()` 对每个库调用经 `EnsureNativeStub(sig)` 造签名驱动的 isNative stub（`OP_CallFunc → CallNative`），**不区分该函数是否 native**。
- nide 过期检测（`MainWindowBuildRun.cpp:191`）：standalone run 仅比较主源 `.n` 与 `.nmod` 的 mtime，未纳入库 `.n`。

### 已用实验确认的缺口（红灯）

- 纯 NLang 库（`int twice(int x){...}`）：编译通过，运行时被当 native，报 `cannot find native module 'nlang_mylib2.dll'`。
  根因：库 `.n` 函数体从不被解析/编译；codegen 一律造 native stub。

## 3. 核心设计

### 3.1 库源完整解析并内联（关键改动）

让搜索路径上的库 `<name>.n` 与项目源一样被 `ScriptParser` **完整解析为一个 TranslationUnit**（含函数体），并在 `MergeTransUnits()` 合并进 AST 根；普通函数体随之参与编译，native 声明保持 `NF_Native`。

- 新增「库 TU」概念，与「项目 TU」区分（`ModuleRegistry::ModuleEntry` 增加 `isLibrary` 标志）：
  - **不**作为项目模块：不进入 wildcard/同目录隐式可见集合（`IsProjectModule()` 排除库 TU），不允许项目目录与库同名的既有保留段守卫仍生效。
  - 占一个正常的 owner/module index（库函数 merge 后 owner 指向该库 TU），保证调试归属与重复定义检查有主。
- 编译顺序调整（`ModuleBuilder::Build()`）：
  1. `ParseTransUnits()`：解析项目源（得到 imports）。
  2. **`DiscoverLibraryUnits()`（新，替代旧 `DiscoverLibrarySources()` 的索引职责）**：以 **worklist 迭代到不动点**——初始为项目 TU 的单段 import；每轮在搜索路径定位 `<name>.n`，对每个尚未完整解析的文件：(a) `LoadFileOnce()` 索引签名（供补全/文档/LSP）；(b) `ScriptParser.ParseUnit()` 完整解析为库 TU，追加到 `m_upTransUnits`，并将**该库 TU 自身的单段 import 加入 worklist**（第三方库可依赖第三方库）。"已内联集合"保证迭代终止。
     - 循环依赖（库 A、B 互相 import）：所有库 TU 与项目 TU 在同一次编译中**先完整 merge、后统一解析**，声明在解析期彼此可见，故允许循环（与项目多文件 TU 一致，类似一次编译内的多源文件），无需前向声明或特殊处理。
  3. `RegisterUnits()`：注册项目 TU + 库 TU（库 TU 置 `isLibrary`）。
  4. `ExpandTypeAliases()`、`LoadImports()`：`BuildImportGates()` 因签名索引识别库命名空间而开门（`gate.builtins`）；`.nmod` 外部模块照常加载。
  5. `MergeTransUnits()`：合并全部 TU（含库 TU），普通库函数体进入根。
- 去重与"已索引但未内联"：完整解析的去重使用**独立的"已内联库文件"集合**，不沿用 `SymbolIndex::m_loadedFiles`。原因：标准库在 `BuildEnvironment` 构造时已由 `LoadLibraryDir()` 索引签名（此时 `IsLibraryNamespace("io")` 已为 true），但并未完整解析；若仅因"已索引/IsLibraryNamespace"就跳过，标准库将永远不内联。故判定标准是"该文件是否已完整解析为库 TU"，与签名索引状态无关；同一库 `.n` 被多处 import 只完整解析一次。
- **可见性隔离**：库 TU 不得触发同目录隐式可见（D7）。`BuildGate()` 遍历 `m_modules` 自动加入同目录模块的循环（现跳过 `isExternal`）须**同时跳过 `isLibrary`**；库 TU 自身的 gate 同样不自动纳入同目录项目模块（库不应反向依赖消费方；库自身的 `import` 仍正常进入 `gate.builtins`，供库函数体内调用其他库）。
- **两种分发形态统一于函数级判定**：源库（`.n`，可查看/修改/重编）与预编译库（`.nmod`，无源码分发）都支持 native 混合——运行时 `OP_CallFunc` 按 `CompiledFunction.isNative` 分派（native→`CallNative`→DLL，否则执行字节码）。`.nmod` 路径（`LoadExternalModule`）天然保留这一能力；Phase 4 的主要补齐工作在 `.n` 源路径的普通函数编译。

### 3.2 resolver 统一走 AST 函数查找

库函数既已内联进根命名空间，库命名空间调用改为在 **AST 命名空间成员**中解析（复用项目模块的 `MatchInvokeAgainst()` 路径），而非签名索引：

- `TryResolveNamespaceStdLibCall()` 调整：gate 校验 import 后，在根中取该命名空间、按名查 AST 函数并 `MatchInvokeAgainst()`：
  - 普通函数（有体）→ 绑定为普通 callee，编译函数体。
  - `NF_Native` 函数（无体）→ 绑定 native callee，codegen 走 `FillNativeFunctionRecord()`（isNative record）→ `CallNative` → DLL。
- 签名驱动的 `EmitStdLibCall()`/`EnsureNativeStub()` 在所有库（含标准库）改为内联后**删除**；`SymbolIndex` 仍保留，职责收敛为「文档/补全/签名提示 + 库命名空间识别」，不再驱动 codegen。
- 不建立"第三方走 AST、标准库走签名"的过渡双路径：内联机制对标准库与第三方完全相同，4a 即让**所有库同时完整内联**、resolver 统一走 AST，签名驱动的 `EmitStdLibCall()`/`EnsureNativeStub()` 在 4a 直接删除（避免为过渡态维护硬编码分支）。标准库行为不变由全量回归兜底；`kStdLibTable` 本体的删除作为独立清理（4c），与内联解耦。
- **消除 `any` 与 print 专用 codegen**：完整 bison 文法不识别 `any`（它只是 SymbolIndex 轻解析的占位），而 `EmitStdLibArgToString()` 是 io.print 独有的 int/float/array/func→string 转换；二者都与"native 函数统一走 AST"冲突。已实验确认通用 string 形参隐式接受 int/float（普通 `take(42)`/`take(3.14)` 编译运行），故重定义 `io.print` 为 `native void print(string s)`、删除 `any`（含 `TypeKind::Any` 与 print 专用转换）：int/float 经通用 string 形参转换；array/func/class/enum 统一要求显式 `.toString()`（与现有 class/enum 诊断建议一致，更符合主流语言）。此为用户可见收窄，CHANGELOG/文档注明，并核对受影响 e2e。
- **函数注册须用全限定名**：现 `RegisterFunctions()` 对 namespace 内函数设 `cf.name = func.Name()`（裸名，`Register.cpp:291`），仅适配 9f 裸名进程内 native；限定名 native 现靠 `EnsureNativeStub` 造 "ns.fn"。内联后库 native 函数经 RegisterFunctions，必须改为按 namespace 路径构造全限定名（顶层自由函数仍裸名；class 方法沿用方法机制），否则 `CallNative` 以裸名解析、找不到 DLL。新增全限定名构造 helper（沿 parent 累加 `NK_Namespace` 段，namespace 可嵌套）。

### 3.3 淘汰 `kStdLibTable`

- 删除 `ExprResolverStdLib.cpp:209` 的 `FindStdLibFunction()` 编译期校验：native 实现统一在运行时解析（与第三方一致），编译期不验证实现存在。
- 删除 `StdLib.h` 的 `kStdLibTable`、`StdLibEntry`、`FindStdLibFunction()` 及仅服务于它的映射。
- **保留**「标准库命名空间保留名」列表（`io/math/fs`，用于 `FindReservedSegment()` 防止项目目录占用），从 `kStdLibTable` 抽离为独立常量（如 `kReservedLibraryNames`）。
- `stdlib/*.n` 由「自动生成」改为**手写源文件**：改写文件头（去掉 DO NOT EDIT / Auto-generated），保留并完善开发者文档注释；native 声明形态不变。
- ctest `stdlib_generation` 与 `check_stdlib_generation.py`：失去存在意义，改为「`stdlib/*.n` 声明与对应 `nlang_*.dll` 导出一致性」的轻量校验，或整体移除（实施时按最简决定，倾向移除生成器、必要的一致性由现有 native e2e 覆盖）。

### 3.4 自动重编译

- **ncc**：每次 build 都完整重解析库 `.n`（源内联模型天然保证），无需额外缓存逻辑。
- **nide**：
  - standalone run 的过期检测（`MainWindowBuildRun.cpp:191`）扩展为：`.nmod` 缺失，或其 mtime **早于任一参与编译的源**（主源 + 经搜索路径发现并内联的库 `.n`）→ 重编。
  - 项目 Build 由 ncc 负责（ncc 已每次重解析），nide 无需另判。
  - 库源打开/跳转：go-to-definition 已能定位 `SymbolInfo.filePath:line`；补充「在编辑器中打开库 `.n`」的路径（F12/跳转直接打开该文件并定位行）。
- DLL（native 实现）改动不触发宿主重编（只影响运行，重新运行即可）——与 C/Python C 扩展一致。

### 3.5 配套

- **ndb**：库函数内联进 `.nmod`，per-function `sourceFile` 记录库 `.n` 绝对路径；断点（`file:LINE`）/步进（s/n）/`l`（SourceCache）需能解析库源文件。验证进入第三方库与标准库源码的调试闭环。
- **nide**：库源可在编辑器中正常打开、编辑、保存（不强制只读——用户对其文件系统上的库 `.n` 有修改权；安装目录若只读由 OS 在保存时报错），保存更新 mtime 即触发重编；提供库源打开/跳转；搜索路径配置（Phase 3d 已就绪）。
- **范围界定**：v1 的限定名访问面为「函数调用 `ns.fn(...)`」与「类型引用 `ns.Type`」（库内 class/struct/enum 随完整内联自然可用，4b 加用例确认）；「限定名值访问」（库全局变量/常量 `ns.CONST`）现状即不在 v1（见 `TryResolveModuleCallTarget` 注释），Phase 4 不扩展。
- **文档（zh/en）**：混合库编写指南（native 声明 + NLang 实现 + DLL 打包布局）、源改动重编行为、查看库源；更新标准库章节（去掉「自动生成/kStdLibTable 过渡」描述）。
- **LSP 预留**：`SymbolIndex`（纯 STL）+ 库 TU 完整 AST 解析均与具体工具解耦；将来 LSP 复用同一发现/解析/签名链路，IDE 语义分析不内嵌 nide 专有逻辑。

## 4. 分阶段计划（小步 TDD，每阶段全量 ctest 绿）

- **4a 统一库源完整内联**：库 TU 完整解析（worklist 含传递依赖）/merge/owner（isLibrary）；resolver 库调用统一走 AST（普通函数编译 body、NF_Native → DLL）；删除 `EmitStdLibCall()`/`EnsureNativeStub()`。标准库与第三方在本步同时内联。
  - 测试（先红后绿）：纯 NLang 库 e2e（库函数被真实调用，含多函数/递归/同库互调）；库 TU owner/去重/可见性隔离；未 import 不可见、import 后可见；**全量回归**（标准库内联后行为逐字节不变）。
- **4b 混合库（native + NLang）**：扩展第三方 fixture，库内 NLang 函数调用 native 函数、native 与 NLang 共存/重载。
  - 测试：混合 fixture 真实编译 + 真加载 DLL + 真执行；普通函数包装 native（如 `int quad(int x){ return native_dbl(native_dbl(x)); }`）；库内定义 class/struct/enum 并被消费方以限定类型名使用（验证完整内联对类型同样生效）。
- **4c 淘汰 `kStdLibTable`**（内联已在 4a 完成）：删除 `FindStdLibFunction()` 编译期校验、`StdLib.h` 的表/条目、stdlib 手写化（去自动生成文件头、完善开发者注释）、抽离保留名常量、处理 ctest `stdlib_generation` 与生成器。
  - 测试：全量回归仍绿；保留名守卫（项目目录占用 io 被拒）；删除项全局无残留引用。
- **4d 自动重编译 + 配套**：nide 过期检测纳入库 `.n`（QtTest 集成）、库源打开/跳转；ndb 进入库源（machine e2e）；中英文档 + 翻译；按 AGENTS.md 提升 VERSION 次版本并在 CHANGELOG（中英成对）记录；source_size_guard；循环完善。

## 5. 测试策略

- 严格 TDD：每个能力先写会失败的真实用例（编译器真编译 `.n`、VM 真执行字节码、真加载 DLL，不 mock）。
- 复用/扩展现有：`test_thirdparty.cpp`、`test_native_loader.cpp`、fixture `tests/fixtures/native/sample/`；新增混合 fixture。
- nide 改动以 QtTest 集成测试覆盖（参考 Phase 3d `test_searchpath_integration.cpp` 模式）。
- 覆盖率目标 >90%；每用例短超时。

## 6. 风险与对策

- **库 TU 内联导致重复定义/命名冲突**：复用 `MergeFrom`（命名空间可跨 TU 合并）与 `DuplicateFieldChecker`；库与项目同名由保留段/同库池守卫拒绝。
- **顺序调整影响 alias/import 解析**：库发现提前到 RegisterUnits 前，确保签名索引在 gate 构建前就绪；alias 预遍历对库 TU 默认无 alias（若有则同机制处理）。
- **删除 kStdLibTable 误伤 string 内建方法**：string 12 个接收者方法仍走 `OP_CallIntrinsic`（不在库命名空间，本阶段不动）；仅删 math/io/fs 命名空间相关表项，逐引用核对。
- **跨平台/部署**：库 `.n` 与 `nlang_*.dll` 的搜索路径已统一（Phase 3d）；安装包需随附 `stdlib/*.n`（现状已随附 stdlib，核对）。
- **调试器找不到库源**：`sourceFile` 用绝对路径，SourceCache 三级解析兜底；缺失时给出可读提示而非崩溃。

## 7. 提交

- 分阶段英文 Conventional Commits；不提交 `AGENTS.md`、`build-dev/`、`temp/`；master 不动；不推送。
- 提交前循环完善（审核-验证-完善，直到审核无新问题）；按 AGENTS.md 提交前确认（用户已预先授权自主推进，仍于交付时汇报）。
