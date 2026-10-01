# Phase 6 运行期包加载与链接 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把「整项目合并单 `.ncu`」改为「逐源文件 `.ncu` ＋ 符号引用 ＋ `.npkg` 归档 ＋ nvm 运行期加载与链接」，使第三方包可独立分发与升级（规格：`docs/dev/phase6_loader_design.md`）。

**Architecture:** ncc 对第三方 `.npkg` 签名表做类型检查、产出携带符号引用表的 `.ncu` v2.0 并打包 `.npkg` v1.0；nvm 的包加载器急切加载导入闭包、统一解析全部符号引用并回填解析表槽位，错误在任何用户字节码前一次报清；跨模块访问经解析表间接寻位（GOT/PLT 模式，热加载使能结构）。

**Tech Stack:** C++17（MSVC 2019）、CMake＋CTest、Python（e2e runner 与打包冒烟）。

**Spec:** `docs/dev/phase6_loader_design.md`（本计划逐任务论证自该规格；执行者须同读）。

## Global Constraints

- 不做向下兼容：`.ncu` 格式 **2.0**、`.npkg` 格式 **1.0**，旧文件一律拒绝并指名两方版本号。
- 扩展名与魔数只用常量：`NCU_EXTENSION`（`".ncu"`）、`NCU_MAGIC[8]`（`"NLANGCU\0"`），禁止长度算术（用 `fs::path::extension()`）。
- 每任务一次提交、一次转绿；提交后 `git status --porcelain` 只允许 `?? main.n`、`?? .zcodeignore`。
- 全量 ctest 串行；e2e 基线 **977/6**（六条失败逐字为既有集，SKIP=0）；docs **67 passed**；源尺寸守卫 clean。新测试使条目数超过 64 时，以「当时实测值」为准写进该任务门并同步后续任务的期望。
- 错误处理：加载/链接诊断在任何用户字节码执行之前完成，**一次报清**（不报首条即停）。
- 术语：文件级身份叫「包」（package）；`.ncu`＝编译单元制品；`.npkg`＝包归档。
- stdlib 本阶段维持源码内联，不制作运行期 stdlib 包。
- 公开文字零旧引擎痕迹；诊断措辞与规格 §6 逐字一致。

## Review Focus

1. 旧 v1.13 `.ncu` 喂给 nvm → 必须以版本诊断拒绝（指名两方版本号），不得崩溃或误读。测试：Task 1 的版本负例。
2. `.ncu` 成员字节被篡改 → 加载拒绝并指名包与模块（校验和不符）。测试：Task 7 的篡改负例。
3. 缺第三方 `.npkg` 运行 → 启动期一次报清全部缺包＋搜索路径列表，任何用户字节码不执行。测试：Task 5 的缺包子进程负例。
4. 循环包依赖（A→B→A）→ 双方都加载、程序正常执行，不得死循环或重复加载。测试：Task 5 的循环正例。
5. 同一 `.npkg` 内重复成员路径 → 打包与加载两端都拒绝。测试：Task 2 的归档负例。

---

### Task 1: `.ncu` 格式 2.0——头部加模块路径，版本双抬

**Files:**
- Modify: `include/nlang/vm/CompiledModule.h`（`NCU_FORMAT_MAJOR = 2`、`MINOR = 0`；`CompiledModule` 增加 `std::string modulePath;`）
- Modify: `src/vm/ModuleSaver.cpp`（头部写法：magic、major、minor、模块路径 u16 长度＋字节；`entryPoint` **保留**——过渡字段，Task 5 迁移时移除）
- Modify: `src/vm/ModuleLoader.cpp`（对称读取；floor＝`< 2.0`、ceiling＝`> 2.0`，诊断指名两方版本号）
- Modify: `src/compiler/ModuleBuilder.cpp`／ncc 产码侧（所有 `.ncu` 产出点设置 `modulePath = 输出模块名`——过渡期合并形态即输出模块名；Task 3 起为真实单元路径）
- Test: `tests/test_vm/test_debugger.cpp`（版本钉子改 2.0：floor 负例补丁字节按新头部布局重算）、`tests/test_vm/test_library_source.cpp`（读回 `mod.modulePath` 断言）

**Interfaces:**
- Produces: `CompiledModule::modulePath`（后续任务按它建符号解析的归属）；加载器 2.0 地板/天花板语义（Task 2 的 `.npkg` 成员加载复用同一 ModuleLoader）。

- [ ] **Step 1: 写失败测试**——`test_debugger.cpp` 版本钉：fresh build 头部 `major==2 && minor==0`；floor 负例（major 补丁为 1）断言旧文案「is outdated; recompile with current ncc」；ceiling 负例（major 补丁为 3）断言「newer ncc」文案；`test_library_source.cpp` 加 `CHECK(mod.modulePath == <输出模块名>)`。
- [ ] **Step 2: 跑测试确认失败**（当前 1.13：版本断言与 modulePath 均红）。
- [ ] **Step 3: 实现**——常量、Saver/Loader 对称读写、全部 `.ncu` 产出点补 `modulePath`；诊断文案按规格 §6 保持「指名两方版本号」。
- [ ] **Step 4: 全量门**——ctest 串行全绿（版本钉已同步）、e2e 977/6 逐字、docs 67、守卫 clean。
- [ ] **Step 5: Commit**——`git add <显式清单>`；`feat(vm): .ncu format 2.0 - module path in header, version floor/ceiling at 2.0`。

### Task 2: `.npkg` v1.0 归档——写端、读端、命令面

**Files:**
- Create: `include/nlang/vm/NcuPackage.h`、`src/vm/NcuPackage.cpp`（`NcuPackageWriter`、`NcuPackageReader`）
- Modify: `include/nlang/vm/CompiledModule.h`（`NCU_EXTENSION` 旁新增 `NPKG_EXTENSION = ".npkg"`）
- Modify: `src/tools/ncc/main.cpp`（项目模式：obj 目录写逐成员 `.ncu`＋`outputDir/<proj>.npkg`；`ncc run x.npkg` 分支）
- Modify: `src/tools/nvm/main.cpp`（`.npkg` 入参：读成员表→按入口点/单成员加载）
- Modify: `tests/CMakeLists.txt`（新加载器测试条目；期望条目数以实测为准并写回本计划）
- Test: `tests/test_vm/test_ncu_package.cpp`（新建）

**Interfaces:**
- Produces: `struct EntryRecord { std::string modulePath; std::string functionName; };`、`NcuPackageWriter::AddMember(modulePath, ncuBytes)`、`Write(path, const EntryRecord*)`（库传 `nullptr`）；`NcuPackageReader::Open(path)`、`MemberPaths()`、`ExtractMember(modulePath) -> bytes`、`EntryRecord()`（程序包；库为空）；`uint64_t NcuChecksum(const char*, size_t)`（**FNV-1a 64**——规格把算法选择交给本计划，此处定死，阶段 7 复用）。
- 归档布局：头部（magic `"NLANGPKG"`、版本 1.0、包名、标志位、签名块描述符＝算法 0/偏移 0/长度 0、成员数、可选入口点记录）＋成员表（路径→偏移/长度/`NcuChecksum`，按路径排序）＋成员字节。

- [ ] **Step 1: 写失败测试**——`test_ncu_package.cpp`：①写→读往返（两成员，读回字节相等）；②成员按路径排序（乱序 Add，MemberPaths 有序）；③重复成员路径 → Open/Write 拒绝并指名；④校验和篡改 → Extract 拒绝并指名包与模块；⑤程序包入口点记录往返；⑥魔法数/版本负例。
- [ ] **Step 2: 跑测试确认失败**（类型未定义，编译失败即 RED）。
- [ ] **Step 3: 实现**——按 Interfaces 的布局写 Writer/Reader；校验和用 FNV-1a 64；成员排序在 Write 内做。
- [ ] **Step 4: ncc/nvm/nide 接线**——项目模式产 `.npkg`；`ncc run x.npkg` 与 `nvm x.npkg` 走 Reader→Extract→既有 ModuleLoader；**nide 项目运行/调试路径同步改 `.npkg`**（`MainWindow.cpp` 运行组合、`SettingsStore.cpp:73/:92` 的项目产物名），`nide_deploy_check` 与相关单测随改。单文件模式不变（`build src.n` 仍出单 `.ncu`，`ncc src.n` 编译＋执行不变）。
- [ ] **Step 5: 全量门**——ctest 全绿（新增条目数写回此处与 Task 3+ 的期望）、e2e 977/6、docs 67、守卫 clean。
- [ ] **Step 6: Commit**——`feat(vm): .npkg v1.0 archive writer/reader; ncc project mode packs it`。

### Task 3: 逐单元产码＋运行期函数链接（本阶段核心，风险最高）

**Files:**
- Modify: `src/compiler/ModuleBuilder.cpp`（项目模式：跳过 `MergeTransUnits` 的代码gen前合并；各 TU 依 stub 签名各自过 `VmBackend`）
- Modify: `src/compiler/builder/ModuleRegistry.cpp`／`ModuleRegistryGate.cpp`（项目内兄弟单元经「签名桩」互见——复用既有外部导入桩机制）
- Create: `src/compiler/builder/SymbolReferenceTable.hpp`（符号引用表：`(类别, 目标包路径, 名字, 形参数)` 记录，字节码操作数引用记录下标）
- Modify: `src/vm/backend/VmBackend.cpp`（跨模块调用发符号引用；`.ncu` 尾部附引用表）
- Create: `src/vm/NcuLinker.cpp` ＋ `src/vm/NcuLinker.h`（**内存模式**：对加载进内存的 N 份模块映像统一解析；文件路径与进程内两条加载路共用它）
- Modify: `src/tools/nvm/main.cpp`／`src/tools/ncc/main.cpp`（nvm：加载 `.npkg` 全部成员→NcuLinker→执行；**ncc 进程内编译＋执行路径（e2e 语料的执行通道）同样改走 NcuLinker**——否则全量 e2e 在本任务必红）
- Modify: `tests/e2e/run_e2e_tests.py`（多文件用例「运行最后一个 `.ncu`」改为带测试目录的包搜索路径，使加载器闭包加载兄弟 `.ncu`）
- Test: `tests/test_vm/test_thirdparty.cpp`（端到端关键能力专项）、`tests/test_vm/test_ncu_package.cpp`（链接诊断负例）

**Interfaces:**
- Consumes: Task 1 的 `modulePath`；Task 2 的 Reader/Writer；阶段 5 的限定表键（运行期函数注册键已唯一，跨成员查找即键查找）。
- Produces: 解析表槽位语义（「一条符号记录一个槽位，首解析回填」）——Task 4 的类型槽复用同一结构。

- [ ] **Step 1: 写失败测试（端到端，子进程门）**——`test_thirdparty.cpp` 新专项 `TestProgramPackageWithoutSources`：两个项目（lib 出 `lib.npkg`、app `import lib;` 只拿 lib.npkg 无源码）→ ncc 编译 app 成功（签名表）、nvm 运行成功并断言输出；同场景删 lib.npkg → 运行期缺包诊断一次报清。
- [ ] **Step 2: 跑测试确认失败**（当前 app 编译期就合并源码，无 .npkg 消费路径）。
- [ ] **Step 3: 实现分三步、每步 ctest 保持绿**：
  1. 产码去合并（stub 化兄弟单元；本单元直接下标、跨单元发符号引用；类型表暂保持运行期全局平铺——阶段 5 键唯一）；
  2. `.npkg` 成员从 1 变 N；
  3. NcuLinker 解析函数符号（eager，一次报清；**内存模式**由 nvm 文件加载与 ncc 进程内执行两条路共用）。
  每步跑全量门；红了先修再前进，不携带红提交。
- [ ] **Step 4: 全量门**——ctest 全绿（含新条目）、e2e 977/6 逐字、docs 67、守卫 clean。
- [ ] **Step 5: Commit**——`feat(vm): per-unit codegen with runtime function linking`。

### Task 4: 类型与字段的运行期定址（去类型平铺）

**Files:**
- Modify: `src/compiler/builder/SymbolReferenceTable.hpp`（类别=类型/字段；字段按「类型槽位＋字段名」）
- Modify: `src/vm/NcuLinker.cpp`（类型槽＝目标 CompiledClass 下标与字段偏移表；字段名→偏移定址）
- Modify: `src/vm/backend/{Import,Types,EmitExprNew,EmitExprInitList,EmitStmtAssign,EmitStmtDecl,EmitStmtForeach,EmitStmtStore,EmitStmtSwitchTry}.cpp`（原类型下标操作数改符号记录下标——阶段 5 站点表同族；`Import.cpp` 的 remap 链整体退役，见 Step 3）
- Test: `tests/test_vm/test_thirdparty.cpp`（跨包类型布局独立性负例：同末段两包各自 `Rec` 布局互串即算错——Task 5 已有 `(6)`，此处加 `.npkg` 形态重复）、`tests/test_vm/test_ncu_package.cpp`（缺类型/字段名不符的链接诊断）

**Interfaces:**
- Consumes: Task 3 的解析表结构与 NcuLinker 骨架。
- Produces: 类型/字段全部经解析表定址（运行期不再有全局类型平铺）。

- [ ] **Step 1: 写失败测试**——`.npkg` 形态的同末段两包 `Rec` 布局隔离（正例）＋「包在、字段名不在」的链接诊断（负例，指名包/模块/字段）。
- [ ] **Step 2: 确认失败**（当前类型平铺 + 下标重映射路径）。
- [ ] **Step 3: 实现**——类型符号化＋链接期字段名定址；`Import.cpp` 的 remap 链
  （`RemapBytecode`＋`PerModuleRemap`）随类型符号化**整链退役**——外部单 `.ncu`
  导入与项目单元统一走运行期加载路径，不再有第二套编译期重定位（裁定，不留两可）。
- [ ] **Step 4: 全量门**（同 Task 3）。
- [ ] **Step 5: Commit**——`feat(vm): symbolic type and field references resolved at load`。

### Task 5: 入口点迁移＋闭包/循环/缺包子进程门

**Files:**
- Modify: `include/nlang/vm/CompiledModule.h`、`src/vm/{ModuleSaver,ModuleLoader}.cpp`（`.ncu` 移除 `entryPoint`——Task 1 的过渡字段）
- Modify: `src/tools/ncc/main.cpp`（`.npkg` 头部入口点记录：`.nproj` 指定入口源文件，缺省 `main.n`；单文件模式默认包一层 `.npkg`——`ncc run src.ncu` 保留为直接执行退化形，**默认入口＝自身模块＋函数 `main`，缺失即入口诊断**）
- Create: `tests/e2e/` 子进程门用例（缺包、循环依赖 A↔B、入口缺失三种，manifest 同步）
- Test: `tests/test_vm/test_ncu_package.cpp`（入口点记录负例）

**Interfaces:**
- Produces: 程序/库 `.npkg` 唯一区别＝头部入口点记录（规格 §3）。

- [ ] **Step 1: 写失败测试**——循环正例（A↔B 两包互调，输出断言）；缺包子进程负例（诊断一次报清＋搜索路径列表）；入口缺失负例；`.ncu` 无入口点后单文件 `ncc run` 直执行退化形正例。
- [ ] **Step 2: 确认失败**。
- [ ] **Step 3: 实现**——入口点搬 `.npkg` 头；`.ncu` 2.0 收紧；e2e 三条 manifest 行。
- [ ] **Step 4: 全量门**——e2e 期望 **980 通过 / 6 失败**（977＋3 条新用例；以实测写回）、ctest 全绿、docs 67、守卫 clean。
- [ ] **Step 5: Commit**——`feat(vm): entry point moves to .npkg header; closure subprocess gates`。

### Task 6: native 收口＋确定性＋安全层 1

**Files:**
- Modify: `src/vm/backend/VmExecutorNativeHost.cpp`（DLL 按**顶层包段**命名 `nlang_<首段>.dll`，解除单段限制；符号＝完整限定名）
- Modify: `src/vm/NcuPackage.cpp`／`NcuLinker.cpp`（加载期逐成员 `NcuChecksum` 校验；不符拒绝并指名包与模块）
- Modify: `tests/packaging/verify_package.py`（冒烟加 `.npkg` 形态：build→run→校验和篡改负例）
- Test: `tests/test_vm/test_ncu_package.cpp`（多段包 native 正例——Task 5 的 `native_multi` 诊断翻正）、篡改负例

**Interfaces:**
- Consumes: Task 2 的 `NcuChecksum`；规格 §10 第 1 层（完整性校验）。

- [ ] **Step 1: 写失败测试**——`vendor/graphics` 多段包 `native` 从编译诊断翻为可用（DLL 名 `nlang_vendor.dll`）；篡改 `.ncu` 字节 → 加载拒绝指名包与模块。
- [ ] **Step 2: 确认失败**。
- [ ] **Step 3: 实现**——NativeHost 命名规则改首段；加载期校验和。
- [ ] **Step 4: 全量门**。
- [ ] **Step 5: Commit**——`feat(vm): native packages by top-level segment; load-time checksums`。

### Task 7: 文档与门收口

**Files:**
- Modify: `docs/user_manual/{en,zh}/language-spec/packages.md`（加载/链接时机、`.npkg` 形态、诊断表）、`{en,zh}/vm-architecture/compilation-pipeline.md`、`cli-tools/{ncc,nvm}.md`
- Modify: `CHANGELOG.md`、`CHANGELOG.zh-CN.md`（0.7.4 条目补阶段 6）
- Test: docs 门（67＋新措辞）

- [ ] **Step 1: 文档改写**（en/zh 成对；诊断逐字引用随 §6）。
- [ ] **Step 2: docs 门**（67 passed，tree parity 含改动页）。
- [ ] **Step 3: 全量门**。
- [ ] **Step 4: Commit**——`docs: runtime loading and linking (en+zh)`。

---

## 计划审核记录（用户规则：审核无新问题才进入实施）

### 第 1 轮（单进程内三视角：代码事实／计划自洽／测试风险）

**接受的（逐条对树核实后才改）**：

| 来源 | 结论 | 落到 |
| --- | --- | --- |
| 代码事实 | e2e runner 对多文件用例本就「逐文件编 `.ncu`、运行最后一个」——去合并后运行最后 `.ncu` 必须带包搜索路径，否则闭包加载不到兄弟单元；且 ncc 进程内编译＋执行是 977 条语料的执行通道 | Task 3 Files 补 `run_e2e_tests.py` 与 ncc 进程内路径；NcuLinker 明确**内存模式**两路共用 |
| 代码事实 | nide 以 `.ncu` 组合项目运行/调试路径（`MainWindow.h:381` 注释、`SettingsStore.cpp:73/:92`） | Task 2 Step 4 补 nide 面＋`nide_deploy_check` 随改 |
| 计划自洽 | Task 4 对 `Import.cpp` remap 的处置留了两可（「以实测为准」）——违反「一步一个合理动作」 | 裁定**整链退役**（外部 `.ncu` 导入统一走运行期加载），Files 补 `Import.cpp` |
| 测试风险 | Task 5 的 e2e 期望「977+3 行」混淆行与用例数 | 改「980 通过/6 失败（以实测写回）」 |
| 计划自洽 | Task 2 Interfaces 未定义 `EntryRecord` 字段 | 补 `struct EntryRecord { modulePath; functionName; }` |
| 测试风险 | Task 5 直接执行 `.ncu` 的默认入口规则未写 | 补「自身模块＋函数 `main`，缺失即入口诊断」 |

**驳回的**：

- 「Task 3 应同时落类型符号化」→ 类型平铺在阶段 5 唯一键上是安全的过渡态（Task 4 拥有它），合并会撑爆单任务的保持绿边界。
- 「Task 2 就该把单文件也包进 `.npkg`」→ 单文件直执行是既有语料形态，包装属 Task 5 的入口点迁移面，提前做只会双倍返工。

### 第 2 轮（复审修订稿：只查修订自身引入的不一致）

两处小问题当场修正：①Task 4 Files 补 `Import.cpp`（Step 3 已裁定退役，清单漏列）；②Task 3 Step 3.3 措辞改「内存模式由 nvm 文件加载与 ncc 进程内执行两条路共用」。

**第 2 轮结论：无新增发现。** 审核循环结束，进入实施（Task 1 起）。

## Self-Review 记录

- **Spec 覆盖**：§2（T3/T4）、§3（T2/T5）、§4（T3/T5）、§5（T6 native、T7 文档）、§6（T5/T6 诊断）、§7（T2/T5 命令面）、§8（各任务测试步＋T3 端到端专项）、§9（T1/T2）、§10 第 1 层（T6）——第 2/3 层按规格只做格式预留（T2 头部描述符），无实现任务，符合「轻量化」裁定。
- **类型一致**：`NcuChecksum`/`NCU_EXTENSION`/`NPKG_EXTENSION`/`modulePath`/解析表槽位在 Tasks 间同名词。
- **Review Focus**：五行各自落点见 Task 1（版本）、Task 6（篡改）、Task 3/5（缺包）、Task 5（循环）、Task 2（重复成员）。
