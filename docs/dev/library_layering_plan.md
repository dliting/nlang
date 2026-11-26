# 库分层重构（nlang_core提取）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: 用 superpowers:executing-plans（主线自做＋终局fresh reviewer）逐任务实施本计划。步骤用checkbox（`- [ ]`）跟踪。

**Goal:** 消除nlang_vm↔nlang_compiler的库级环形依赖，按spec定稿拓扑P3提取公共底座`nlang_core`，全程行为零变化。

**Architecture:** 终局链形`nlang_runtime ← nlang_core(模块格式与镜像) ← {nlang_compiler, nlang_vm}`，langservice保持叶子。core只依赖runtime＋STL；字节码后端57文件迁入`src/compiler/codegen/`；`TypeArgSpelling`与`TypeDesc`两个混合文件按函数级拆家，AST无关半边入core，根治`CanonicalArgSpelling`符号穿边。

**Tech Stack:** C++17、CMake 3.16+、VS2022、Python 3.13（py313 conda环境）、CTest。

**Spec:** `docs/dev/library_layering_design.md`（提交12125457）——冲突以spec为准。锚点：分支`refactor/nlang-core`，起点59aad4ed。

## 全局约束（spec逐条摘录）

- 行为零变化：语言面、标准库面、诊断文案、退出码、ABI（含`.ncu`/`npkg`格式）五类用户可见面全部不变；用户手册正文不动。
- core准入判据：`src/core/`与`include/nlang/core/`的include面只许`nlang/runtime/`与STL；出现AST、builder、调度器include即违规。spec§六断言1–5全部成立：include类断言（1–3）一律锚`#include`指令行（注释含库名不算命中）；断言4–5扫描两库`CMakeLists.txt`的链接段与include段文本行（spec§六限定）。
- 不向下兼容：include路径直接改`nlang/core/`，不留兼容别名。
- 测试只用真实组件；现有断言一字不改，预期改动止于include行与链名表；新增ctest条目按`tests/README.md`新增测试约定补`tests/gate_time_reference.txt`各构型参考行。
- 注释英文；LF无BOM；提交信息英文Conventional Commits；`git commit -F`前剥BOM；`CLAUDE.md`/`AGENTS*.md`/`docs/superpowers/`等本地独占文件绝不git add。
- 版本号0.13.12a（用户裁定），`CHANGELOG.md`与`CHANGELOG.zh-CN.md`成对登记。
- 门禁判决只认`tools/gate.sh`判决行与退出码；每任务收尾走T0影响映射的Debug层（构型级改动保守取Debug全量单测＋受影响e2e族）。
- 本会话（dev线）子代理并发≤1；临时脚本放`temp/`，可复用设施放`tools/`。

## 评审关注点（spec未言明、最可能咬人的五类）

1. **`.ncu`字节漂移**：拆并翻译单元可能扰动格式序列化输出（静态初始化顺序、常量折叠语境），ABI类「格式不变」承诺落空。钉在任务1基线快照＋任务7逐字节对拍（Expected：diff为空）。
2. **ndisasm反汇编回归**：`OpCodeTable`静态表随core迁移，反汇编文本若因链接布局变化漂移，ndisasm用户面受损。钉在任务1快照＋任务7对拍（ndisasm全量反汇编输出逐行diff）。
3. **link-only兜底消失后的undefined symbol**：现状nvm/ndisasm靠CMake把vm的PRIVATE compiler依赖以`$<LINK_ONLY>`附进链名表才解析`CanonicalArgSpelling`；新拓扑删声明后兜底消失。钉在任务6的nvm/ndisasm实测链接＋守卫断言5。
4. **裸名include被目录搬移切断**：`VmBackend.h`（4个测试目标）、`Ncu*`（ncc/nvm/ndb/nlang/embed/tests）等裸名消费随迁移断链。钉在任务5/6的确定性grep盘点＋全树全量构建（编译通过即钉）。
5. **embed对外面回归**：`NLang.h`纯STL、8个公开头拆后`include/nlang/vm/`仅剩NativeHost×2，对外ABI句柄面须零变化。钉在任务3的`NLang.h`与embed include零改动检查＋`test_embed_vm`绿。

---

### 任务1：基线快照（无提交）

**Files:**
- Create: `temp/core_snap.py`（快照脚本，Write落文件后执行）
- Create: `temp/core_split_baseline/`（快照产物＋`manifest.sha256`）

**Interfaces:**
- Consumes: 锚点12125457的干净树（任何代码改动之前）。
- Produces: 基线`.ncu`字节集、ndisasm反汇编文本、`manifest.sha256`，供任务7对拍。

- [ ] **Step 1：基线工具树构建**（E盘紧张，放F盘隔离目录，防增量构建并发污染）

```bash
cmake -S . -B F:/nlang-builds/dev-base-core -G "Visual Studio 17 2022" \
      -DNLANG_BUILD_DOCS=OFF \
      -DFLEX_EXE=D:/dev/win_flex_bison/flex.exe -DBISON_EXE=D:/dev/win_flex_bison/win_bison.exe
cmake --build F:/nlang-builds/dev-base-core --config Release --target ncc ndisasm nlang_stdlib_package
```
（tests/README.md L24-25形态；测试项`NLANG_BUILD_TESTS`缺省不开；文档项缺省ON但基线树无docs面须点名关闭——根`CMakeLists.txt:358`要求`python`在PATH可跑docs管线；VS多配置树不传`CMAKE_BUILD_TYPE`，`--config Release`选构型；Flex/Bison路径为build-dev缓存实测值。）
Expected: ncc/ndisasm产出且无警告级新增（既有告警不计），且`F:/nlang-builds/dev-base-core/stdlib/stdlib.npkg`产出——`.npkg`由根`CMakeLists.txt:324-331`的ALL custom target `nlang_stdlib_package`以`ncc build -p stdlib.nproj`打包，显式`--target`不构建ALL目标故须点名；产物位置=根`CMakeLists.txt:260`的`${CMAKE_BINARY_DIR}/stdlib`。

- [ ] **Step 2：快照脚本**：对`stdlib/`全部`.n`与`examples/`全部`.n`逐个执行`ncc build <src.n> -o <out.ncu>`（命令形态取`tests/check_ncu_determinism.py:62`既有配方），产物落`temp/core_split_baseline/ncu/`；`ndisasm <package.npkg>`（CLI形态见`src/tools/ndisasm/main.cpp:170`）对基线树`stdlib/stdlib.npkg`（位置见Step 1）全量反汇编落`temp/core_split_baseline/disasm/`；`manifest.sha256`=全部产物相对路径＋SHA-256清单。
Expected: 语料数＝`stdlib`与`examples`下`.n`文件总数；manifest非空。

- [ ] **Step 3：快照可复现自检**：同基线树二次快照到`temp/core_split_baseline_rerun/`，与首快照diff为空。
Expected: 空diff（否则先查快照脚本自身的非确定性，如临时路径混入字节）。

本任务无提交（快照在`temp/`，不入库）。

---

### 任务2：分层守卫脚本与注册（RED承载）

**Files:**
- Create: `tools/layering_guard.py`（stdlib-only，无pytest）
- Create: `tests/fixtures/layering/`（按断言扫描面逐对生成fixture共8对：断言1一对、断言2两个扫描面各一对、断言3一对、断言4两侧各一对、断言5两工具各一对；每对violation＋clean）
- Modify: `tests/CMakeLists.txt`（仿`gate_time_check_selftest`注册先例L3422-3435；`add_test`经L85-86 wrapper统一挂临时目录/TIMEOUT/资源锁）
- Modify: `tests/gate_time_reference.txt`（两条新条目各构型参考行）

**Interfaces:**
- Consumes: 无（只读仓库）。
- Produces: CLI契约`layering_guard.py [--selftest] [--repo-root PATH]`：默认对仓库跑断言1–5，违规逐行打印`ASSERT<n> <file>:<line> <snippet>`并以1退出，零违规退出0；`--selftest`跑fixture自检恒0。ctest条目`layering_guard`（本任务起挂`WILL_FAIL TRUE`，任务6摘除转绿）与`layering_guard_selftest`（恒绿）。

断言模式表（断言1–3锚`#include`行；断言4–5扫CMakeLists文本行）：

| 断言 | 扫描面 | 违规模式 |
|---|---|---|
| 1 | `src/vm/**.{h,cpp}` | `"nlang/compiler/`、`<nlang/compiler/`、`"builder/`、`"codegen/`（`"codegen/`是compiler树新目录面的include镜像，防任务5后回流） |
| 2 | `src/core/**.{h,cpp}`＋`include/nlang/core/*.h` | `"nlang/compiler/`、`<nlang/compiler/`、`"builder/`、`"nlang/vm/`、`<nlang/vm/`、`"VmExecutor`、`"VmScheduler` |
| 3 | `src/compiler/**.{h,cpp}` | `"nlang/vm/`、`<nlang/vm/` |
| 4 | `src/vm/CMakeLists.txt`、`src/compiler/CMakeLists.txt` | vm侧含`nlang_compiler`或`${PROJECT_SOURCE_DIR}/src/compiler`；compiler侧含`nlang_vm`或`${PROJECT_SOURCE_DIR}/src/vm` |
| 5 | `src/tools/nvm/CMakeLists.txt`、`src/tools/ndisasm/CMakeLists.txt` | 链名表含`nlang_compiler` |

- [ ] **Step 1：fixture先写**（RED测试面）：每条断言的violation fixture含一行命中该断言扫描面的形态（断言1–3为include行、断言4–5为CMakeLists文本行，与上表扫描面逐一对齐），clean fixture干净。
- [ ] **Step 2：跑脚本确认RED**：`--selftest`8对fixture全判定正确；裸跑当前树必命中断言1（vm→compiler约59文件）与断言4（vm借目录行`:177`、链接行`:182`、compiler`:194`借目录行，共3行），退出1。
Expected: 违规清单非空且含`src/vm/backend/`成员；断言5今日即通过（nvm/ndisasm链名表本就不列compiler，符号兜底靠`$<LINK_ONLY>`，静态面测不到——实测链接证明归任务6）。
- [ ] **Step 3：注册ctest条目**：`layering_guard_selftest`普通注册；`layering_guard`注册并`set_tests_properties(layering_guard PROPERTIES WILL_FAIL TRUE)`。
- [ ] **Step 4：跑ctest确认两条目状态**：`ctest --test-dir build-dev -C Debug -R "layering_guard" -I 1,2`。
Expected: 两条目均PASS（其一靠WILL_FAIL）。
- [ ] **Step 5：参考行回填**：按`tests/README.md` L99约定——各构型先真实跑一遍两条目产出junit，再`tools/gate_time_check.py record --config <构型> --from-junit <junit文件>`按构型逐次回填（`--config`为必填参数），`check-complete`腿通过。
- [ ] **Step 6：提交**

```bash
git add tools/layering_guard.py tests/fixtures/layering tests/CMakeLists.txt tests/gate_time_reference.txt
git commit -m "test(gate): add layering guard entries ahead of the core split"
```

---

### 任务3：公开头树迁移与include路径重写（纯搬家，构型不变）

**Files:**
- Move（git mv）→`include/nlang/core/`：`include/nlang/vm/`的CompiledModule.h、CompiledModuleRecords.h、ExprDesc.h、TypeDesc.h、Intrinsics.h、NcuPackage.h六头；`include/nlang/compiler/TypeArgSpelling.h`；`src/vm/`的BytecodeOps.h、BytecodeStrides.h、BytecodeEmitter.h、BytecodeReader.h、ModuleLoader.h、GenericInstDescriptor.h、Disassembler.h七头（共14头）。
- Modify：全仓include行（清单由下列确定性grep产生）；`tools/source_size_guard/allowlist.json`旧路径键（如含上述头）。

**Interfaces:**
- Consumes: 任务2守卫（此时断言1/4仍RED属预期）。
- Produces: 公开头终局集合14个（上列），路径`<nlang/core/X.h>`；`include/nlang/vm/`此后仅剩NativeHost.h、NativeHostHelpers.h。闭包事实（已核实）：`Disassembler.h:3-4`、`BytecodeEmitter.h:2`、`BytecodeReader.h:2`、`BytecodeStrides.h:9`对`BytecodeOps.h`等同目录兄弟引用以引号相对形式天然闭合；唯二跨目录行`Disassembler.h:2`与`ModuleLoader.h:2`的`"nlang/vm/CompiledModule.h"`改写为同目录引号兄弟形式。

include重写规则（三组grep全仓出全清单，逐行改写，不许漏）：
1. `#include ["<]nlang/vm/(CompiledModule|CompiledModuleRecords|ExprDesc|TypeDesc|Intrinsics|NcuPackage)\.h` → `nlang/core/`同名。
2. `#include ["<]nlang/compiler/TypeArgSpelling\.h` → `nlang/core/TypeArgSpelling.h`。
3. 裸名`#include "(BytecodeOps|BytecodeStrides|BytecodeEmitter|BytecodeReader|ModuleLoader|GenericInstDescriptor|Disassembler)\.h"` → `<nlang/core/同名>`（`include/nlang/core/`目录内同目录兄弟保持引号相对形态）。

- [ ] **Step 1：git mv 14头**，随后修正`Disassembler.h:2`、`ModuleLoader.h:2`两行同目录化，`GenericInstDescriptor.h:8`改`<nlang/core/TypeArgSpelling.h>`。
- [ ] **Step 2：按三组grep改写全仓include行**（src/include/tests全扫；tests改写不算断言改动）。
- [ ] **Step 3：残留哨兵**：三条grep再跑，Expected: 全部空命中。
- [ ] **Step 4：全量构建＋受影响单测**：build-dev（Debug）与build（Release，e2e二进制）增量构建；`layering_guard_selftest`绿。构型未变、仅include搬家→跑受影响单测族（守卫fixture与格式、disasm族）＋`test_embed_vm`（评审关注点5的embed钉）。
Expected: 构建零错误；受影响族判决全PASS。
- [ ] **Step 5：`include/nlang/embed/NLang.h`与`src/embed/`对外include零改动核对**（NLang.h本就不引vm/compiler；embed内部改动仅限公开路径重写）。
- [ ] **Step 6：提交**

```bash
git add include src tests tools
git commit -m "refactor(core): move module format and bytecode vocabulary headers to nlang/core"
```

---

### 任务4：混合文件函数级拆家（构型不变）

**Files:**
- Create: `src/compiler/builder/CanonicalArgSpelling.cpp`（core半，任务6迁`src/core/`）
- Create: `src/vm/TypeDescBuild.cpp`（AST半，任务5迁`src/compiler/codegen/`）
- Modify: `src/compiler/builder/TypeArgSpelling.cpp`（删core半）、`src/vm/TypeDesc.cpp`（删`BuildTypeDesc`）、`src/compiler/CMakeLists.txt`、`src/vm/CMakeLists.txt`（各登记新文件一行）、`tools/source_size_guard/allowlist.json`（豁免条目`(src/vm/TypeDesc.cpp, BuildTypeDesc)` L119-121的path改写为`src/vm/TypeDescBuild.cpp`、reason不变——`BuildTypeDesc`含签名223行（原242-464），豁免落空则`source_size_guard`条目当场红）

**Interfaces:**
- Produces:
  - `CanonicalArgSpelling.cpp`承载`CanonicalArgSpelling`（原40-59行）、`HasTypeParamArg`（原398-410行）两函数，include仅`<nlang/core/TypeArgSpelling.h>`＋runtime（`RTK_Array`在`RuntimeTypeCodes.h`，已随头引入）＋STL（core准入达成，为任务6符号穿边根治就位）。`HasTypeParamArg`体内调用的`IsTypeParamArg`是头内inline（`TypeArgSpelling.h:114`），无需随行搬移。
  - `TypeArgSpelling.cpp`保留其余14个AST依赖函数（`IsTypeMember`、`EnclosureMethodKey`、`QualifiedName`、`LinkKeyOf`、`PackageQualifiedName`、`DescriptorSpellingOf`、`ClassArgOf`、`InterfaceArgOf`、`ArrayArgOf`、`LeafArgOf`、`BuildTypeArgOf`、`BuildTypeArgsOf`×2）＋匿名namespace helper`ArraySpelling`（原29-37行；唯一调用者是留守的`ArrayArgOf`:294，匿名namespace不跨TU，必须随留守半走）。
  - `TypeDescBuild.cpp`承载`BuildTypeDesc`（原242-464行）并承接原文件整个include块：行10/12（`TypeDesc.h`、`CompiledModule.h`，任务3已改`nlang/core/`路径）、行13/16/17（AST三头，其中行17`SyntaxNodeConsts.h`另供`NodeKindDebugName`用于459，定义在该文件:115）、行14`BuiltinGenericNames.h`（`kBuiltin*`用于378/389/404）、行15`StackDump.h`（`NLANG_ASSERT`宏即该头提供，456行使用）＋STL。`TypeDesc.cpp`剩wire编解码与解析，恰好删去13–17五行（`kBuiltin*`/`NodeKindDebugName`/`NLANG_ASSERT`实测仅出现在`BuildTypeDesc`体内；wire半保留行12的RTK_*与STL），include净。声明不动（仍在`include/nlang/core/TypeDesc.h`，AST前置声明形态）。

- [ ] **Step 1：拆`TypeArgSpelling.cpp`**（纯搬移零改写，git mv式剪切；搬移前后函数体逐字节相同）。
- [ ] **Step 2：拆`TypeDesc.cpp`**（同上）。
- [ ] **Step 3：全量构建＋受影响单测**：拼写族（`test_descriptor_spelling`、`test_generic_descriptor`）、类型desc族（`test_stride_table`、`test_func_records`）＋保守Debug受影响族。
Expected: 全PASS（行为纯搬移，任何红都是搬移错位）。
- [ ] **Step 4：守卫`--selftest`绿＋裸跑仍仅命中断言1/4**（本任务不解决环，只备料）。
- [ ] **Step 5：提交**

```bash
git add src/compiler src/vm tools/source_size_guard/allowlist.json
git commit -m "refactor(core): separate AST-dependent definitions from wire spelling and type-desc TUs"
```

---

### 任务5：字节码后端迁compiler（codegen目录）与ICodeBackend退役

**Files:**
- Move（git mv）：`src/vm/backend/`全部57项→`src/compiler/codegen/`；`src/vm/VmBackend.h`→`src/compiler/codegen/VmBackend.h`；`src/vm/TypeDescBuild.cpp`→`src/compiler/codegen/TypeDescBuild.cpp`。
- Modify: `src/compiler/CMakeLists.txt`（源表增`codegen/*.cpp`段，`PRIVATE` include目录增`${CMAKE_CURRENT_SOURCE_DIR}/codegen`）；`src/vm/CMakeLists.txt`（删backend段、`VmBackend`/`TypeDescBuild`条目、link段行182内摘除`nlang_langservice` token——行182实为`PRIVATE nlang_compiler nlang_langservice ${CMAKE_DL_LIBS}`单行，不可整行删，丢`${CMAKE_DL_LIBS}`会致Linux dlopen潜伏回退）；`include/nlang/compiler/ICodeBackend.h`（删除）；`src/compiler/ModuleBuilder.cpp`、`include/nlang/compiler/BuildEnvironment.h`、`src/compiler/BuildEnvironment.cpp`、`src/compiler/codegen/VmBackend.h`（接口退役）；`src/compiler/builder/BindingTable.h:8-9`（过期注释当场修正）；`tools/source_size_guard/allowlist.json`（`backend/WalkersMaxArgs.cpp`豁免、任务4落的`TypeDescBuild`豁免与`src/vm/VmBackend.h`文件级豁免（L23，实测2706行超FILE_MAX_LINES=500，豁免按repo相对路径全等匹配）三条path键改写`codegen/`前缀）与`tools/test_families.txt`（6条backend族src规则路径改写）。
- Modify（tests）：裸名`"VmBackend.h"`消费目标（`test_builtin_binding`、`test_generic_typeparam_gc`、`test_interface_rtk_gc`、`test_ref_ops`，全仓grep补全）加`${PROJECT_SOURCE_DIR}/src/compiler/codegen`到include目录（include行不动）。

**Interfaces:**
- Consumes: 任务4的`TypeDescBuild.cpp`（消费者codegen的Register*/GenerateUnits，同库化）；任务3的`<nlang/core/...>`路径。
- Produces: `src/compiler/codegen/`目录；`VmBackend`成为compiler库内具体类（无接口基类），`ModuleBuilder.cpp:190-194`构造点直调`OnModuleCreate`；vm不再链langservice（EmitCall已出境）。

- [ ] **Step 1：git mv三组**＋两侧CMake源表搬移（顺带修`src/vm/CMakeLists.txt:80,88`的`RegisterStructs.cpp`重复条目：新表只列一次）。
- [ ] **Step 2：ICodeBackend退役**：`VmBackend.h:2`删include、类声明删基类与`override`；`BuildEnvironment`持`ICodeBackend*`的成员/访问器类型改`VmBackend*`（无消费者的成员删除）；`ModuleBuilder.cpp`构造点保持直调。全仓grep `ICodeBackend`，Expected: 零命中（含注释；`ICodeBackend.h:5`的LlvmBackend过期注释随文件消失）。
- [ ] **Step 3：include面收敛**：codegen内对builder/compiler头的裸名与相对include原样有效（同库目录树在路径上）；`ModuleBuilder.cpp:20`等compiler侧裸名`"VmBackend.h"`靠codegen目录入径保持裸名。
- [ ] **Step 4：全量构建＋受影响族**：Debug全量单测（构型级改动，保守）＋受影响e2e族（ncc编译管线语料）。
Expected: 构建零undefined symbol；判决行全PASS。
- [ ] **Step 5：守卫裸跑**：本任务后断言1的57个vm侧含边成员（backend 55＋`VmBackend.h`＋`TypeDescBuild.cpp`）全部出境，断言1应零命中；断言2仍真空通过（`src/core/`未建）；断言4仍命中（vm借目录行`:177`、链接行`:182`、compiler`:194`借目录行）。
Expected: 断言1零命中，断言4命中清单恰为上述三行CMake行。
- [ ] **Step 6：提交**

```bash
git add src/vm src/compiler src/tools tests tools
git commit -m "refactor(compiler): move the bytecode backend to src/compiler/codegen and retire ICodeBackend"
```

---

### 任务6：nlang_core建库与双向边界封死（守卫转GREEN）

**Files:**
- Create: `src/core/CMakeLists.txt`
- Move（git mv）：`src/vm/`24个`.cpp`入`src/core/`——OpCodeTable、GenericInstDescriptor、TypeDesc、TypeDescIndices、ModuleLoader×4（Records/RecordsInterfaces/RecordsClasses/主文件）、ModuleSaver×5、NcuPackage、NcuLoader、NcuLinker×4（主/Remap/Slots/Interfaces）、Disassembler×5（主/Prim/Walk/Format/Calls）；`src/compiler/builder/CanonicalArgSpelling.cpp`入`src/core/`（共25个`.cpp`）；私有头`ModuleLoaderRecords.h`、`ModuleSaverRecords.h`、`NcuLoader.h`、`NcuLinker.h`、`NcuLinker{Interfaces,Remap,Slots}.h`、`Disassembler{Calls,Format,Prim}.h`（如存在其余Disassembler头一并核`ls src/vm/Disassembler*`入列）。
- Modify: 根`CMakeLists.txt`（`add_subdirectory(src/core)`插于langservice之后compiler之前，L222-245区）；`src/vm/CMakeLists.txt`（删25源条目；link段`PUBLIC nlang_runtime`改`PUBLIC nlang_core`；include段删`${PROJECT_SOURCE_DIR}/src/compiler`借目录行与`Reverse precedent`注释L174-177；link段行182内摘除`nlang_compiler` token，行剩余`${CMAKE_DL_LIBS}`保留）；`src/compiler/CMakeLists.txt`（link段`PUBLIC nlang_runtime`改`PUBLIC nlang_core`，注释注明runtime经core传递；删`:194`借`src/vm`行）；`src/tools/{ncc,nvm,ndb,nlang}/CMakeLists.txt`与`src/embed/CMakeLists.txt`（include目录增`${PROJECT_SOURCE_DIR}/src/core`）；`src/tools/ndisasm/CMakeLists.txt`（删借目录行；链名表`nlang_vm`→`nlang_core`）；`tests/CMakeLists.txt`（全部含`nlang_vm`的链名表在`nlang_vm`之后显式加`nlang_core`，含Qt族与手工排序注释目标，L190-192/L720-721/L2269-2271注释同步；裸名`"Ncu*"`/`"ModuleLoaderRecords"`等消费目标加`src/core`借目录，grep确定性产出清单）；`tools/source_size_guard/allowlist.json`（随迁9条`src/vm/`path键改写`src/core/`：ModuleLoader.cpp×2、ModuleLoaderRecords.cpp×2、ModuleLoaderRecordsClasses.cpp、ModuleSaver.cpp、ModuleSaverRecordsClasses.cpp、ModuleSaverRecordsFuncs.cpp、DisassemblerWalk.cpp）。
注意：`src/core`25个被迁文件保持test_families未映射（`tools/test_families.txt`头部curation规则以保守为先：宽叶如compiler frontend、module loader/linker、the opcode dispatcher一律stays OUT、T0保守回退全量Debug单测；disasm/Ncu族现状即无族规则），**不新增**族映射行——新增行会人为收窄T0影响映射。

**Interfaces:**
- Consumes: 任务3公开头路径、任务4core半文件、任务5后端出境后的vm残体。
- Produces: `nlang_core`静态库（PUBLIC导出`include/`根＋`nlang_runtime`，PRIVATE本目录）；vm/compiler互不借目录互不链接；nvm实测无compiler链接通过（评审关注点3钉）。spec「tests各套件链名表补core」的落实范围：含`nlang_vm`的目标全部显式加列（vm PUBLIC core）；纯compiler侧套件经`nlang_compiler`PUBLIC core已传递解析，不强改全表——裁决注记入ledger。

- [ ] **Step 1：git mv＋根/两侧CMake改写＋core CMakeLists**（`add_library(nlang_core ...)` 25源；include目录`PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>`＋`$<INSTALL_INTERFACE:include>`＋`PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}`；`target_link_libraries(nlang_core PUBLIC nlang_runtime)`。install面维持不装库不装头）。
- [ ] **Step 2：消费者适配**（上列链名表/借目录七文件＋tests批量）。nvm/ndb/ncc/nlang/embed裸名`Ncu*`消费改借目录即可，include行不动；ndisasm的main.cpp三行已在任务3改公开路径，本步只换链名表。
- [ ] **Step 3：vm残体裸名哨兵**：`grep -rn '#include "\(Ncu\|ModuleLoaderRecords\|ModuleSaverRecords\|Disassembler\(Calls\|Format\|Prim\)\)' src/vm`。
Expected: 空命中（若命中：优先改公开路径，确无私有头借`src/core`并加一句理由注释）。
- [ ] **Step 4：实测链接（断言5的链接面）**：全新configure＋Release构建`nvm`与`ndisasm`。
Expected: 零undefined symbol——`CanonicalArgSpelling`经core就位，`$<LINK_ONLY>`兜底消失无感（评审关注点3钉）。
- [ ] **Step 5：守卫转绿**：删`tests/CMakeLists.txt`中`WILL_FAIL TRUE`行；`ctest -R "layering_guard"`两条目PASS，裸跑守卫输出零违规退出0。
Expected: 断言1–5全绿；`GATE`层`layering_guard` PASS。
- [ ] **Step 6：全量门禁**：build-dev Debug全量单测＋受影响e2e族（构型级改动保守全量）＋nide_editor所在IDE树本轮不强求（IDE树全量ctest属收口批，见任务9）。
Expected: 判决行全PASS。
- [ ] **Step 7：提交**

```bash
git add CMakeLists.txt src tests tools
git commit -m "build(core): extract nlang_core and seal the compiler/vm boundary"
```

---

### 任务7：字节级对拍与反汇编对拍（无代码提交）

**Files:**
- Create: `temp/core_split_after/`（重放快照）
- Consume: `temp/core_split_baseline/`（任务1）

**Interfaces:**
- Consumes: 任务6后的HEAD（VERSION仍0.13.12，与基线同版）。
- Produces: 对拍结论（评审关注点1/2钉）。

- [ ] **Step 1：同Release构型增量构建**当前树`ncc ndisasm nlang nlang_stdlib_package`（后者为Step 4重打包与Step 5 nvm冒烟的前提）。
- [ ] **Step 2：重放快照脚本**→`temp/core_split_after/`。
- [ ] **Step 3：对拍**：`diff -r temp/core_split_baseline temp/core_split_after`。
Expected: 空diff。非空即停，走systematic-debugging定位（先钉字节差异位于格式还是内容，再回`git log -p`二分）。
- [ ] **Step 4：`stdlib.npkg`对拍**：两树各自重新打包的`stdlib.npkg`字节比较；若包内混载时间戳/路径噪声（基线可复现自检已排除快照脚本噪声），降级为包内`.ncu`条目逐一字节比较并ledger注记判据。
Expected: 字节一致（或按注记判据一致）。
- [ ] **Step 5：nvm冒烟**：新树`nvm`跑stdlib入口一例退出0（e2e族已覆盖，此步为手工旁证）。

本任务产物在`temp/`，无提交；若对拍暴露缺陷，修复另起提交并全批重扫。

---

### 任务8：版本面与发布登记

**Files:**
- Modify: `VERSION`（0.13.12→0.13.12a，保行尾）
- Modify: `CHANGELOG.md`、`CHANGELOG.zh-CN.md`（成对新条）
- Modify: `docs/user_manual/SYNC_BASELINE`（`tools/manual_sync_check.py --update`推进）
- Modify（本地，绝不git add）: `AGENTS.reference.md`模块边界节按新拓扑改写

**Interfaces:**
- Consumes: 任务7对拍通过。
- Produces: 0.13.12a版本面；手册基线覆盖本批全部提交。

- [ ] **Step 1：VERSION提升**＋根configure复跑确认L14校验`^[0-9]+\.[0-9]+\.[0-9]+[a-z]?$`与L21剥段兼容；`ctest -R "^version_"` PASS（注册条目为`version_ncc`/`version_nvm`/`version_ndisasm`/`version_ndb`/`version_nlang`五条，`tests/CMakeLists.txt:2793-2811`；无匹配rc=8非版本面故障）。
- [ ] **Step 2：CHANGELOG中英成对**新条：库分层重构（新增内部静态库nlang_core、后端迁入compiler、头路径更名nlang/core），明记「无用户可见面变化」。
- [ ] **Step 3：手册判定与基线**：py313跑`tools/manual_sync_check.py`，基线以来每提交逐个判定——本批全部为构型/内部面，不属五类（语言面/标准库面/诊断文案/退出码/ABI）；`--update`后提交。
Expected: 判定表逐条留痕；刷新后复跑drift为0。
- [ ] **Step 4：AGENTS.reference.md就地改写**（本地独占；模块边界、五库→六库、目录结构节同步）。
- [ ] **Step 5：提交**

```bash
git add VERSION CHANGELOG.md CHANGELOG.zh-CN.md docs/user_manual/SYNC_BASELINE
git commit -m "docs(release): bump VERSION to 0.13.12a with changelog pair and manual baseline"
```
注：VERSION提升会令后续每构型树重建（含docs腿`--master-tree`吃Release），与任务9合跑。

---

### 任务9：收口门禁与挂账清单

**Files:** 无新改动（验证与登记）。

- [ ] **Step 1：影响层门禁**：`tools/gate.sh`默认串行锁跑T0影响映射Debug层（构型级改动→Debug全量单测＋受影响e2e族；守卫两条目天然在列）。
Expected: 判决行全PASS，退出0。
- [ ] **Step 2：release级收口批清单登记**（打tag前一次执行，AGENTS门禁策略）：ASan构型冒烟（内存风险族虽仅构型变动，保守带入）、IDE树全量ctest（编译器/预置包变更所需，含Qt族）、Release构建＋Release e2e同点、慢测试族、手册五腿＋carrier复跑。逐项命令见`tests/README.md`。
- [ ] **Step 3：挂账核对**：ICMP三腿豁免挂账不因本批改动解冻（机器未重启，维持原状）；`docs/dev/README.md`收录spec与plan两行留待所属线合流时点（本会话不触碰该文件的他线改动）。
- [ ] **Step 4：终局fresh reviewer**（executing-plans协议）：review-package覆盖59aad4ed..HEAD全分支，评审判决Critical/Important当场修复（RED→GREEN＋全量重跑），Minor入ledger。

tag、push、PR#1处置、Release与本地部署均属用户确认面，不自行推进。

---

## 附：终局形态速查（任务链产物）

```
nlang_runtime ← nlang_core ← { nlang_compiler(+codegen, +langservice), nlang_vm }
include/nlang/core/  14公开头     src/core/  25 .cpp＋10私有头
include/nlang/vm/    NativeHost×2  src/compiler/codegen/  backend 57＋VmBackend＋TypeDescBuild
nvm/ndb/ncc/nlang/embed  借 src/core＋src/vm 两目录（裸名Ncu*/vm私有头先例保持）
ndisasm              仅链 nlang_core＋nlang_version，无借目录
tests                链名表含 nlang_vm 者一律显式列 nlang_core（解耦传递）
```
