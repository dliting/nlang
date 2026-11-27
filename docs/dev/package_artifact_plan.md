# NLang包产物编译期消费与标准库拆包（F1）实施计划（v0.13.12）

> **For agentic workers:** 本计划按任务逐个执行（superpowers:executing-plans内联执行）。步骤用checkbox（`- [ ]`）跟踪。

**目标：** 落地docs/dev/package_artifact_design.md（五稿，提交bfc0ce02）定义的包产物编译期消费（点分/单段/通配/传递闭包/折叠）与标准库按领域拆包，含容器格式1.1的包内native成员。

**架构：** 三步依赖序（spec§四）——拆分改名（D4）先行独立可停靠；编译期点分产物消费（D2/D3/D6/D7）与拆分正交同版交付；包内native成员（D5）纯增量收官。编译期管线按发现/门/装载三阶段重构：发现相位（DiscoverLibraryUnits扩展）把产物臂镜像读进待装载集并跑不动点，external条目在RegisterUnits之后、BuildImportGates之前物化，门查条目开门，装载循环消费待装载集做桩合成。

**技术栈：** C++17/CMake、Flex/Bison不动（零文法变更）、编译器导入管线（ModuleBuilder族）、NcuPackageReader/Writer（容器）、NcuLoader/NativeLibraryLoader（运行期）、ModuleLoader（.ncu线格式）。

**Spec：** `docs/dev/package_artifact_design.md`（五稿，提交bfc0ce02）。计划与spec冲突时以spec为准。

**状态：** 终稿（三轮评审收敛：第一轮18项、第二轮7项、第三轮3项Low全吸收，逐条内联标注「评审N」/「评审二轮发现N」/「评审三轮发现N」；三轮终裁「适合进入实施」——二轮7/7真实根治、~40处锚点抽检全命中。二轮Medium清点：embed_host_smoke批边界豁免、native接线六位点＋装载回执形态、注释面grep判据收敛、install-layout语义行归属Task 12）。

## 全局约束（spec逐条摘录）

- **D1成员表真源两端同规则**：编译期产物臂两子臂＝`<dir>/<导入名>.ncu`直文件→逐包成员表扫描（目录序、包内文件名序）；`.npkg`文件名不参与任何解析判定；单段导入名同样按成员表匹配。跨包重复成员→编译期点名两个包文件；运行期维持文件名序取先不动。
- **D2源码优先、产物补缺**：形态级先于目录级——源码臂（任意目录）命中则不落产物臂；产物内部才按目录序。
- **D3.1三阶段相位**：发现＝DiscoverLibraryUnits同一相位（ModuleBuilder.cpp:75调用点），external条目发现期只**定型**（名与镜像入待装载集），`AddExternalModule`物化钉在RegisterUnits之后、BuildImportGates之前（＝LoadImports入口、ModuleBuilderImports.cpp:44之前）；门＝现状位置形态（BuildImportGates:44）查条目开门；装载＝现状LoadExternalModule循环（:49-53）桩合成，merge先于桩不变量保持；循环检查装载后（RejectImportCycles:57）。现状单段乐观开门（ModuleRegistryGate.cpp:96-104）与装载候选收集externalNames（ModuleBuilderImports.cpp:43、:49-53）退役；FindModuleFile文件名臂退役（ModuleBuilderExternalImports.cpp:144-160）；「carries N members」拒绝文案退役（同文件:52-56）；RegisterExternalStubs内`AddExternalModule`（同文件:121）随物化前移退役（评审M2——防ModuleFunctions首命中无桩空条目）。
- **D3.2发射相位**：直接导入名走门期not found臂（ModuleRegistryGate.cpp:108现状路径）；传递引用名（仅镜像导入记录点名）由**发现相位直接发射**，同`ModuleNotFoundText`文案（ModuleRegistry.cpp:73-78）；不静默跳过。镜像解析失败（包可开、成员在、字节坏）按既有「Failed to load module」文案在发现期报告，不并入not-found族。
- **D3.4不可读包**：成员表扫描中Open失败的.npkg不中止扫描，登记清单；not-found报告附注`Note: package 'dir/x.npkg' could not be opened.`（门期臂与发现相位发射臂两处）。
- **D4十三包**：concurrent（thread＋concurrent）/io/math/fs/tensor/net/http/url/system/gzip/regex/json/web（web成员随web线收口）；lang与containers系prelude不产物化（CMakeLists.txt:284-287既有REMOVE_ITEM保持）；「src/与include/零代码字面量」。
- **D5容器1.1**：成员表增「成员种类」字段（单元|native库）；native成员以文件名为键、不参与导入名解析与通配集；运行期开包时抽取到`%LOCALAPPDATA%\nlang\pkg-cache`（包路径哈希目录＋临时名原子改名＋成员校验和守卫）并注入NativeLibraryLoader搜索路径，`EnsureLoaded`懒装载维持；编译期对native成员只在成员表可见零加载。旧读取器遇1.1包拒绝且报错具名版本。
- **D6层裁定**：`HasLibraryArtifact`记忆宿主钉导入管线翻译单元；BuildEnvironment不触碰NcuPackageReader（环境层消费者保持只链环境层）；发现与折叠两个消费者共用一份查询设施（含不可读包登记）。
- **D7通配并集**＝源码子树∪产物成员表子树（精确成员∪前缀成员，native排除）；gate侧**exact与prefix判定面同步**扩展到external条目（通配基名自身作为成员存在时进gate.exact——与源码形态IsCompiledInModule行为一致，非限定可见性两形态不分歧）；空并集诊断文案不变（ModuleRegistryGate.cpp:59-61）。
- **计划停在详细设计层**（用户裁定沿用lambda先例）：任务给锚点/接口/测试形状，不内嵌实现代码全文。
- **工作树共享另一会话**：提交一律逐文件点名；`tests/gate_time_reference.txt`、`docs/roadmap.md`的v0.13.13行、`docs/dev/generic_signature_design.md`不属本计划，绝不暂存。
- 命名遵守repo约定（类型PascalCase、方法/变量camelCase）；注释英文；诊断文案逐字进测试断言。
- 每任务TDD：先写RED测试、亲眼看它失败再实现。
- 每任务收尾跑T0影响映射Debug层（tools/gate.sh默认串行形态，不绕锁）；导入管线/ExprResolver属宽面文件，影响映射未命中时保守回退Debug全量单测。ASan/IDE树/全量e2e/慢测试/手册五腿留收口批（Task 16）。

## 评审关注点（spec未直言、最可能咬人的五类输入）

1. **裸`.ncu`直文件单段导入的现存主路径回归**——`import lib;`经`<dir>/lib.ncu`直文件消费是现存external主路径（test_module_import外部族＋bareCrossModuleRejected:1979-1991现已是正例）；三阶段重构把它从FindModuleFile平移进发现相位时不得断。测试钉在Task 4（回归钉）。
2. **双形态遮蔽序**——同一库陈旧.npkg在前目录＋新源码在后目录：源码必须赢（D2形态级先于目录级）；产物臂绝不能因目录序反超源码。测试钉在Task 11（e2e双形态序）。
3. **迁移期混包**——拆包后遗留旧stdlib.npkg与新领域包并存：同一导入名两包成员表都命中须编译期点名（D1），不得静默取先。测试钉在Task 9。
4. **字节坏镜像的文案分类**——包可开、成员表命中、镜像解析失败：报「Failed to load module」而非「Module not found」（包在、成员在，not-found文案失真）。测试钉在Task 4（4.1(d)）。
5. **容器版本守卫**——1.1包遇1.0读取器拒绝且报错具名版本（不向下兼容）；新读取器必须同时吃1.0与1.1。测试钉在Task 13。

---

### Task 1: CMake打包链按领域拆分

**Files:**
- Modify: `CMakeLists.txt:253-307`（注释:253、构建目录:260-261、glob:275-276、包源列表:284-287、nproj生成:291-297、custom target:298-307）
- Modify: `CMakeLists.txt:437`（install FILES）
- Test: 新增stdlib包布局检查ctest（tests/下注册）

**Interfaces:**
- Produces: 构建树`stdlib/`下13个领域包文件（concurrent/io/math/fs/tensor/net/http/url/system/gzip/regex/json/web各`<domain>.npkg`）；聚合目标名**`nlang_stdlib_package`保持不变**（一项计划级裁定：spec D4把目标名列为配套改名决策点，取「目标名保留、产物拆分」——probe_pair_ab.sh:51、tests/e2e/run_e2e_tests.py:330、tools/gate_freshness.py:184的按名重建提示全部零改动，churn最小且KISS）。`NLANG_STDLIB_BUILD_DIR`（:260-261）与`NLANG_TEST_STDLIB_DIR`（tests/CMakeLists.txt:10）目录级符号语义不变保留。
- Consumes: 无（先行独立）。

**设计形状：**
- 每领域一个nproj＋一个`ncc build -p …/<domain>.nproj -o …/<domain>.npkg`命令，全部挂在既有`add_custom_target(nlang_stdlib_package ALL …)`（:298-307）之下（13条COMMAND、DEPENDS ncc不变）。领域源列表按spec D4表**显式点名**（不glob——确定性且与spec表一一对应）；**web包源＝显式`stdlib/web.n`＋`file(GLOB … stdlib/web/*.n)`子模块臂的并集**（评审M1：web今日是平铺`stdlib/web.n`、无`stdlib/web/`目录——只glob子目录会产出空web.npkg、既有web单元从产物面消失；子模块源按点分词干规则落`stdlib/web/static.n`等，随web线Tasks 8-10落地自动进包，两臂并集才完整，履行spec§四顺序协调句）。lang/containers排除保持（:284-287 REMOVE_ITEM语义随显式列表自然成立）。**陈旧产物清理**（评审M7：CMake不自动删除旧产物，既有构建树的stdlib.npkg会与新13包并存——触发Task 9重复成员诊断、污染布局检查）：custom target增一条幂等删除命令清`${NLANG_STDLIB_BUILD_DIR}/stdlib.npkg`（`cmake -E remove`，各构建树随下次构建自清）；安装前缀与NSIS升级面的旧包残留由1.3部署双保险步核对清除。**声明源拷贝臂裁定**（评审三轮发现3）：`NLANG_STDLIB_SOURCES`的glob（:275-276）与copy_if_different拷贝（:299-301）随拆包一次定型递归形态——源glob扩`stdlib/web/*.n`臂、拷贝保相对路径（`web/static.n`落`${NLANG_STDLIB_BUILD_DIR}/web/static.n`——平铺拷贝会弄断点分模块路径的源臂解析）；此臂与web线Tasks 8-10共享同一CMake区域，递归形态一次到位即解除踩踏。
- nproj生成（:291-297形态）循环13次：Project name＝领域名，成员＝该领域单元；库包无entry record（WritePackageArtifact:209既有「libraries (no entry) pack without a record」路径）。
- install（:437）改为`install(FILES <13个包路径> DESTINATION stdlib)`。
- 门禁参考：`install(DIRECTORY stdlib/ … PATTERN "*.n")`（:432-433）源码分发面不动。

**Steps:**
- [ ] **1.1 写失败测试**：布局检查拆两层（评审I16——python侧无成员表读取面）：(a) python文件集ctest（形态参照tests/check_nlang_tool.py）：构建树`stdlib/`下恰有13个`<domain>.npkg`且`stdlib.npkg`不存在；(b) C++成员表ctest（tests/test_vm族新增或并入test_ncu_package.cpp，经NcuPackageReader读`NLANG_TEST_STDLIB_DIR`真包）：每个包的成员表恰含该领域单元集、无包含lang/containers成员（web包断言「至少含web单元，子模块成员以树内实际为准」——两臂并集语义）。RED＝现状只有stdlib.npkg。
- [ ] **1.2 确认RED**。
- [ ] **1.3 实现**：CMakeLists按设计形状拆分；聚合目标名保留。**部署面双保险核对**（spec§六混包迁移期）：install规则由:437驱动自动正确，另grep核对NSIS/installer脚本与本地部署脚本无`stdlib.npkg`字面量残留（installers/、tools/部署脚本面；残留即随本任务清除——构建树面由custom target的幂等删除命令自清（设计形状），安装前缀/NSIS面由本步核对清除，CMake不自动删除既有产物、评审M7；编译期D1诊断兜住未删净场景）。
- [ ] **1.4 确认GREEN**＋重建`nlang_stdlib_package`目标＋Debug全量单测ctest（**embed_host_smoke除外**——评审二轮发现1：其第三实参`${NLANG_TEST_STDLIB_DIR}/stdlib.npkg`（tests/CMakeLists.txt:3656；NLANG_TEST_STDLIB_DIR＝${CMAKE_BINARY_DIR}/stdlib（:10）——与custom target删除命令同一目录），删除旧包后该实参悬空、本步必红；Task 2.4随实参改目录形态复跑收口）；e2e runner冒烟同留Task 2.4——run_e2e_tests.py:320-332的stdlib包检查此刻仍指旧名单，先跑会假红exit(2)，评审M7；运行期目录扫描已是现状，Debug单测（除上述一项）全绿＋Task 2后e2e绿即证13包解析面。
- [ ] **1.5 提交**：`build(stdlib): split the stdlib distribution package into per-domain containers`（逐文件点名CMakeLists.txt、tests/CMakeLists.txt、新检查脚本）。

### Task 2: 工具脚本与测试装置的包名消费面改名

**Files:**
- Modify: `tools/gate_freshness.py:26,174,184`（新鲜度spec改13包集合：路径:174改包集合、finding文本:184目标名不变零改动、注释:26）
- Modify: `tools/gate_freshness_test.py:5,227,229,239,242,250,252`（fixture与`"stdlib.npkg" in f`字面量断言改领域包形态——spec点名必改项）
- Modify: `tests/e2e/run_e2e_tests.py:301-310,320,330`（stray shadow守卫:301-308本就glob全部`*.npkg`零改动——评审L13，只核对其注释:309-310；stdlib包存在性检查:320-332改13包集合语义；重建提示:330目标名不变）
- Modify: `tests/e2e/run_embed_host.py:5,11,43,53`（参数从单文件改stdlib包目录，:53改拷贝目录下全部`*.npkg`）
- Modify: `tests/e2e/test_runner_timing_gate.py:15-16,94-95,104-106`（stub写入改包集合形态）
- Modify: `tools/debug_assert_smoke.sh:21,67-68`（:67-68从单nproj改循环13个nproj产出workdir全套包）
- 核对（预期零改动）: `tools/http_perf/probe_pair_ab.sh:51`（聚合目标名保留裁定；评审I17——不列为修改项，核对即可）
- Modify: `tests/CMakeLists.txt:3656`（embed_host_smoke第三实参改`${NLANG_TEST_STDLIB_DIR}`目录）
- Modify: `tests/test_embed/test_embed_api.cpp:414-415,611,632,650`（4处，目录语义不变零改动，核对）
- Modify: `tests/test_vm/test_nculoader.cpp:207,357,362`（合成fixture包名改中性名如`lib.npkg`，消除误导性grep残留）
- Modify: `tests/check_nlang_tool.py:12`（注释）
- Modify: 注释/fixture面（评审二轮发现3：2.4的grep清零判据下约12处注释提及随批更新——过期注释当场改纪律；评审三轮发现1补入stdlib声明源两处）：`src/tools/nvm/main.cpp:211`、`src/tools/nlang/ScriptRunner.h:226`（spec D4点名的两处src注释）、`stdlib/lang.n:8`、`stdlib/containers.n:5-6`（「Compile-time inline only: stdlib.npkg excludes this file.」——随install(DIRECTORY stdlib/)交付的用户可见声明源注释，spec D4编辑面盘点漏列、本计划补齐）、`tests/CMakeLists.txt:4,15,3605`、`tests/test_vm/test_stdlib.cpp:100`、`tests/test_vm/test_primops.cpp:292`、`tests/test_vm/test_debugger_machine.cpp:28,50,185,254,514`、`tests/test_vm/test_debugger_common.h:80`＋`test_debugger_common.cpp:95`、`tests/test_embed/test_embed_source.cpp:283,511`、`tests/fixtures/nlang/io_hello.n:2`、`tests/fixtures/embed/io_demo.n:3`
- Modify: `tools/gate_freshness.py:126,140,159-165`＋`tests/e2e/run_e2e_tests.py:322-323`（**源集合glob随包集合改同步扩子目录**——评审二轮发现6：两脚本源面现只扫平铺`stdlib/*.n`，Task 1的web包输入臂含`stdlib/web/*.n`；不扩则web线落地后改`web/static.n`不判web.npkg陈旧；gate_freshness.py的逐模块存在检查:159-165同步）
- Test: 上述脚本各自的自测（gate_freshness_test.py等）＋embed_host_smoke

**Interfaces:**
- Consumes: Task 1的13包布局。
- Produces: 全仓`stdlib.npkg`代码/脚本/测试字面量清零（CHANGELOG与roadmap历史记载除外）。

**Steps:**
- [ ] **2.1 写失败测试**：先改`tools/gate_freshness_test.py`的fixture与断言到13包形态（现断言`"stdlib.npkg" in f`）→ 跑它RED。
- [ ] **2.2 确认RED**。
- [ ] **2.3 实现**：逐文件按Files清单改；run_embed_host.py参数形态变更同步tests/CMakeLists.txt:3651-3659的ctest实参。
- [ ] **2.4 确认GREEN**：gate_freshness自测、embed_host_smoke（含Task 1.4递延项——:3656实参已改目录形态，本步收口其豁免）、test_nculoader全绿；`grep -rn "stdlib.npkg" tools/ tests/ src/ include/ stdlib/`清零（bench脚本自建mnist_mlp.npkg等工程产物名不算）。
- [ ] **2.5 提交**：`test(tools): repoint stdlib package consumers at the domain package set`。
- [ ] **2.6 T0**（评审L12：工具脚本涉门禁设施——影响映射命中gate族则跑其T0腿，未命中跑Debug单测冒烟）。

### Task 3: 手册与README字面量改名（机械批）

**Files:**
- Modify: 手册10页×2语言（en/zh同路径集）：`getting-started/install-layout.md`、`cli-tools/ncc.md`、`cli-tools/nvm.md`、`cli-tools/ndb.md`、`libraries/integrating-nlang.md`、`libraries/embedding-api.md`、`vm-architecture/module-serialization.md`、`vm-architecture/library-mechanism.md`、`specification/03-program-structure/packages.md`、`specification/formats/28-npkg.md`（路径前缀docs/user_manual/{en,zh}/）
- Modify: `README.md:132,226,420`、`README.zh-CN.md:127,208,376`
- Test: 手册页级style单测＋snippets审计＋manual_sync基线流程

**Interfaces:**
- Consumes: Task 1的13包命名。
- Produces: 手册与README的`stdlib.npkg`字面量改领域包集表述（「十三领域包」「标准库包按领域分发」）；语义改写（§3.9.2/§3.9.4/包语义统一）**不在本任务**（留Task 12，须与行为落地同步）。

**Steps:**
- [ ] **3.1 手册sync预检**：跑`tools/manual_sync_check.py`判定基线以来提交的用户可见面，确认本批只承载字面量改名。
- [ ] **3.2 编辑**：10页×2逐页机械改名（中英成对、全称不折行、缩略语共现自查）；README×2三处。**例外**（评审二轮发现4）：install-layout.md的包计数语义行（zh:13/en:15「十四个标准包」）整行留Task 12改写——本任务只改该页其余stdlib.npkg字面量，防「十四个标准包——十三个领域包」半改自相矛盾；3.3的grep清零判据对该行豁免（Task 12收口）。
- [ ] **3.3 验证**：改页立即跑页级style单测＋页级snippets审计；`grep`手册树`stdlib.npkg`清零。
- [ ] **3.4 提交**：`docs(manual): rename stdlib.npkg references to the domain package set`；提交后`tools/manual_sync_check.py --update`刷新基线并核对no drift。
- [ ] **3.5 T0**。

### Task 4: 发现/门/装载三阶段重构＋多成员单段导入

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp`（DiscoverLibraryUnits:331-377扩展产物臂、LoadImports:32-61入口物化、externalNames:43/:49-53退役）
- Modify: `src/compiler/ModuleBuilderExternalImports.cpp`（LoadModuleArtifact:36-63泛化为按导入名查成员表、LoadExternalModule:68-109改消费待装载集、RegisterExternalStubs:111-142**其内部`reg.AddExternalModule(name)`调用（:121）随物化前移退役——评审M2**、FindModuleFile:144-160退役删除）
- Modify: `src/compiler/builder/ModuleRegistryGate.cpp`（ApplyNonWildcardImport:72-109查条目开门、乐观开门臂:96-104退役、「is indexed but its source was not found」:91-93文案「source」扩「source or package」）
- Modify: `src/compiler/builder/ModuleRegistry.h:101-105,271-274,278-281`（externalOut形参族退役）＋`ModuleRegistry.cpp`（AddExternalModule:155-162核对）
- Modify: `include/nlang/compiler/ModuleBuilder.h`（待装载集与缓存成员声明）＋`include/nlang/compiler/BuildEnvironment.h`（opaque缓存成员，见Interfaces）
- Modify: `src/compiler/ModuleBuilderImports.h`（缝头：BuildParams:14前向声明同位置增`struct ArtifactPackageCache;`前向与查询声明）
- Test: `tests/test_compiler/test_module_import.cpp`新增用例＋外部族回归

**Interfaces:**
- Produces: `struct PendingArtifactLoad { std::string name; CompiledModule image; std::string packagePath; uint32_t extIdx; };`（modulePath＋镜像＋来源包路径——Task 9诊断消费packagePath；extIdx＝物化时`AddExternalModule`返回的条目索引——评审M2：装载期桩填充直接索引定位，注册表无按名查条目接口，且ModuleFunctions取首个命中条目（ModuleRegistry.cpp:306-318），留双份条目会让首命中落在无桩空条目、全部限定外部调用解析失败）；ModuleBuilder成员`std::vector<PendingArtifactLoad> m_pendingArtifacts`（**按名去重**——跨TU重复导入同名只物化一次，承接externalNames既有ContainsValue去重语义）。`struct ArtifactPackageCache`（打开的包成员表缓存＋逐名memo＋不可读包清单；BuildEnvironment持有`std::shared_ptr<ArtifactPackageCache>`opaque成员——**前向声明only**，创建点在发现相位（管线TU内make_shared，类型完整），`~BuildEnvironment`不要求完整类型；未跑导入管线的env-only消费者恒null）。物化＝LoadImports入口逐pending调`AddExternalModule(name)`（ModuleRegistry.cpp:155-162），先于BuildImportGates:44。
- Consumes: `NcuPackageReader`（include/nlang/vm/NcuPackage.h:55-76，Open/MemberPaths/ExtractMember）；`ModuleLoader::LoadFromBytes`（src/vm/ModuleLoader.h:17-19，镜像读取——ModuleBuilderExternalImports.cpp:60既有同形态调用）。
- 供Task 5/6/7消费：发现扫描对任意形状导入名（单段与点分）同一算法；Task 8消费ArtifactPackageCache；Task 9消费扫描的命中清单与不可读清单。

**设计形状（发现臂算法，D2/D1序）：**
- DiscoverLibraryUnits既有不动点（源码臂）循环体内：对源码臂未命中的名，逐搜索目录（EffectiveLibraryDirs序）：先`<dir>/<导入名>.ncu`直文件（镜像＝ModuleLoader读文件，读后校验**镜像modulePath与导入名一致**——不一致按「Failed to load module」族报错，与运行期身份规则（NcuLoader.cpp:186-189拒成员名不匹配）同构，堵住「编译过、运行期才报模块不存在」的同形陷阱——评审I18），后逐包扫描该目录`*.npkg`（文件名序）成员表精确匹配导入名（单元成员；Task 13后native成员排除）——首个命中即定型入待装载集，扫描途中**收集全部命中**（供Task 9；**收集辖域＝首个命中目录**——评审三轮发现2：该目录内全包扫完，供Task 9同目录点名与不可读登记；跨目录不再收集、按D2目录序取先，不触发Task 9诊断——Task 11钉的异目录取先是合法形态）与Open失败包（入不可读清单）。**镜像解析失败（包可开、成员在、字节坏）在本任务即分类**——扫描内try/catch按既有「Failed to load module」文案族报告（评审L11：不得把LoadFromBytes的throw路径裸留到Task 6），Task 6只承接传递引用名的发射臂。通配spec的成员表精确∪前缀成员同样进发现集（Task 7接线，本任务先留扫描入口）。
- 门：ApplyNonWildcardImport对单段与点分统一「查注册表external条目」——命中→`gate.exact.push_back(成员模块路径)`；未命中→既有not-found臂（dotted臂:108文案不变；单段臂从乐观开门改为同族not-found——文案不变、时机前移）。**external条目查臂排在`isLibraryNamespace`臂（:89-95）之前**（评审二轮发现5：产物臂已解析成功的名字若先撞「indexed but source not found」早退臂，仍会误报搜索路径错误）。`isLibraryNamespace`臂检查面扩两形态、文案「source」→「source or package」。
- 装载：LoadExternalModule循环改迭代m_pendingArtifacts——RegisterExternalStubs用**缓存镜像**（不再重读盘）并按pending的extIdx定位既有条目填桩（评审M2：条目已在，物化时建）；桩碰撞检查（merge先于桩）现状不变量不动。

**Steps:**
- [ ] **4.1 写失败测试**（test_module_import既有gate-project harness形态，:439-441逐单元打包先例）：(a) 多成员npkg单段导入——搜索目录放**命名为tp.npkg**、含`tp`＋`tp.helper`两单元的包（评审I15：夹具必须叫tp.npkg——现状RED＝FindModuleFile文件名臂命中后「carries N members and no entry record」拒绝，异名夹具RED形态会变成not-found而非多成员拒绝），`import tp;`编译运行；(b) 单段文件名不敏感——包文件名`other.npkg`而成员表含`tp`单元，`import tp;`通过（现状RED＝FindModuleFile只认tp.npkg文件名）；(c) 直文件身份不匹配——`<dir>/tp.ncu`文件而镜像modulePath为`other`→「Failed to load module」族（现状RED＝静默按导入名绑定、运行期才败）；(d) 镜像字节坏——包可开、成员表命中`tp`、成员字节损坏→「Failed to load module」族（现状RED＝异常裸穿或无关报错）。
- [ ] **4.2 确认RED**（逐字核对拒绝文案）。
- [ ] **4.3 实现**：三阶段按设计形状；externalNames/乐观开门/FindModuleFile/多成员拒绝文案/**RegisterExternalStubs内AddExternalModule**五项退役（评审M2）；物化衔接点钉LoadImports入口（extIdx回写pending）。
- [ ] **4.4 确认GREEN**＋外部族全回归（bareCrossModuleRejected:1979-1991等既有外部用例全绿＝评审关注点1的回归钉）＋bison/parser族无波及。
- [ ] **4.5 提交**：`feat(imports): resolve external imports through package member tables in a discovery phase`。
- [ ] **4.6 T0**（导入管线宽面→保守Debug全量单测）。

### Task 5: 点分子模块导入＋V1非限定延续

**Files:**
- Modify: 无新文件——消费Task 4的发现扫描与门臂（点分名同一算法已覆盖；本任务主要是测试钉面＋发现臂对点名直文件`<dir>/<tp.sub>.ncu`的核对）
- Test: `tests/test_compiler/test_module_import.cpp`新增点分external用例族

**Interfaces:**
- Consumes: Task 4全部产物。
- Produces: 行为——`import tp.sub;`（tp.sub藏于tp包内）编译运行；gate.exact记`tp.sub`成员模块路径，非限定使用与源码形态一致（V1第四支，IsBareVisible查gate.exact含属主模块路径——external桩owner路径即成员路径）。

**Steps:**
- [ ] **5.1 写失败测试**：(a) `import tp.sub;`＋限定调用`tp.sub.x()`（现状RED＝门期dotted not-found）；(b) `import tp.sub;`＋非限定`x()`（V1点分延续，镜像bareCrossModuleRejected:1979-1991单段正例的点分兄弟）；(c) 点名直文件形态`<dir>/tp.sub.ncu`。
- [ ] **5.2 确认RED**。
- [ ] **5.3 实现**：核对Task 4扫描对点分名的直文件/成员表两子臂与门臂统一查条目（预期近零新码；有缺口补缺口）。
- [ ] **5.4 确认GREEN**。
- [ ] **5.5 提交**：`feat(imports): dotted submodule imports from package member tables`。
- [ ] **5.6 T0**。

### Task 6: 桩闭包不动点＋传递闭包＋跨镜像循环＋not-found发射臂

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp`（DiscoverLibraryUnits不动点扩：镜像导入记录的新名回环解析）
- Modify: `src/compiler/ModuleBuilderImportCycles.cpp`＋Create: `src/compiler/ModuleBuilderImportCycles.h`（**ImageImportTargets:76-94从匿名命名空间file-local static提升到内部共享头**——评审L14：发现相位（ModuleBuilderImports.cpp）与循环检查两侧同一收集器，五表清单单一真源（正是spec D3.2「表口径对齐」的实现形态）；行为零改动搬家）
- 核对（零改动）: `src/compiler/ModuleBuilderExternalImports.cpp`（「Failed to load module」:85文案族——Task 4已在发现期复用此族分类，装载期桩合成失败维持既有文案）
- Test: test_module_import传递/环/负例用例族

**Interfaces:**
- Consumes: Task 4待装载集与ArtifactPackageCache。
- Produces: 不动点语义——初始集＝TU导入spec经Task 4扫描的pending；迭代＝对每个新镜像读五张槽表的modulePath（ImageImportTargets同口径），未解析名按D2序再解析（源码臂命中→发现相位内联——RegisterUnits未跑合法；产物臂命中→读镜像入集）；定点后：无着落的传递引用名由发现相位直接发射`ModuleNotFoundText`（ModuleRegistry.cpp:73-78文案）＋不可读包附注位（Task 9接线）；镜像解析失败的分类已在Task 4（4.1(d)）落地，不动点对传递引用镜像沿用同文案族。循环图边集＝全部已发现镜像边，RejectImportCycles:123-141装载后跑（现状顺序不动）。

**Steps:**
- [ ] **6.1 写失败测试**：(a) 传递闭包正例——只`import tp.sub;`，tp.sub签名引用`tp.core.Type`，编译运行（现状RED＝tp.core未加载、签名解析失败）；(b) 传递依赖not-found负例——包缺`tp.core`成员→报`Module 'tp.core' not found. Check the project Sources list or -I import path.`且无签名级联（现状RED＝静默跳过后级联报错）；(c) 跨包循环负例——两包互导→编译期报环（对齐手册§3.9.3承诺）。（镜像字节坏用例已随Task 4.1(d)落地——评审L11分类前移。）
- [ ] **6.2 确认RED**（逐字文案）。
- [ ] **6.3 实现**：ImageImportTargets提升共享头＋不动点迭代＋发射臂（失败分类沿用Task 4既有）；dev-doc同步：phase6_loader_design.md§9末「后续决策项」了结登记（D3裁定引用）。
- [ ] **6.4 确认GREEN**。
- [ ] **6.5 提交**：`feat(imports): stub-closure fixed point over package import targets`。
- [ ] **6.6 T0**。

### Task 7: 通配并集产物臂

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp`（DiscoverLibrarySubtree:274-320旁增产物成员表子树臂——精确∪前缀成员进发现集）
- Modify: `src/compiler/builder/ModuleRegistryGate.cpp`（ApplyWildcardImport:32-68的prefix匹配扩external条目；`!entry.isExternal`过滤退役；**exact判定面同步扩**——评审M3：通配基名自身作为外部成员存在时进gate.exact，与源码形态IsCompiledInModule行为一致（spec D7.2「exact与prefix的判定面同步扩展」）；漏exact会使`import tp.*;`对含精确成员tp的产物包不授非限定可见性、两形态静默分歧）
- Test: test_module_import通配产物用例

**Interfaces:**
- Consumes: Task 4发现扫描入口（通配形态）、物化后external条目。
- Produces: 并集＝源码子树∪产物成员表子树；native成员不进通配集（Task 13后）；空并集诊断「No modules matched import '…*'. …」文案不变（:59-61）；通配仍不授非限定名（IsBareVisible只认gate.exact——external桩在注册表不改变这一点）。

**Steps:**
- [ ] **7.1 写失败测试**：artifact-only包（有.npkg无源码）`import tp.*;`非空并集＋限定调用`tp.sub.x()`可达（现状RED＝「No modules matched import」硬错）；**通配精确成员**——包含`tp`与`tp.sub`两成员，`import tp.*;`后非限定调用tp成员函数可见（镜像源码形态IsCompiledInModule正例——评审M3：漏exact臂则两形态非限定可见性分歧）；空并集负例文案回归钉。
- [ ] **7.2 确认RED**。
- [ ] **7.3 实现**：发现侧∪收集＋gate侧prefix扩条目；循环侧ExpandWildcard:46-57用全部注册路径零改动（确认性核对）。
- [ ] **7.4 确认GREEN**。
- [ ] **7.5 提交**：`feat(imports): wildcard union includes package member-table subtrees`。
- [ ] **7.6 T0**。

### Task 8: 诊断折叠第四臂（产物可知性）

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp`＋`src/compiler/ModuleBuilderImports.h`（`HasLibraryArtifact`定义与缝头声明——BuildParams:14前向声明同位置）
- Modify: `src/compiler/builder/ExprResolver.h:1762-1766`（`KnowsLibraryModule`包装器加第四臂`|| HasLibraryArtifact(m_Env, name)`；include缝头）
- Test: `tests/test_compiler/`折叠用例（e383e1e7源码形态测试族的产物形态兄弟）＋`tests/test_langservice/test_library_index`链接守卫

**Interfaces:**
- Consumes: Task 4的ArtifactPackageCache（BuildEnvironment的shared_ptr成员）＋EffectiveLibraryDirs（src/compiler/BuildEnvironment.cpp:71-80——评审L9：:107-119是HasLibrarySource）。
- Produces: `bool HasLibraryArtifact(const BuildEnvironment& env, const std::string& name);`（自由函数，管线TU定义：逐目录扫包成员表、走cache的逐名memo——发现与折叠一份设施）。五个消费位点经`KnowsLibraryModule`单包装器（ExprResolverQualified.cpp:51/78/110/150、ExprResolverTypes.cpp:130——位点本身零改动，spec D6「五处」指HasLibrarySource批的布线，第四臂只动包装器）。层不变量：BuildEnvironment.h/.cpp不include NcuPackageReader；env-only链接（test_library_index）保持可链接。

**Steps:**
- [ ] **8.1 写失败测试**：无import＋artifact-only包上`tp.sub.Fn.f()`→折叠到`Module 'tp.sub' is not imported. Add 'import tp.sub;' (or 'import tp.*;')`（现状RED＝三臂全败按整链点名）；`import tp;`后穿类链的既有源码形态行为不回归。
- [ ] **8.2 确认RED**。
- [ ] **8.3 实现**：缝头声明＋包装器接线；cache点查询惰性扫描（null cache＝无导入管线跑过→false，语义正确）。dev-doc同步：name_resolution_design.md D-29三臂→四臂带日期修订。
- [ ] **8.4 确认GREEN**＋test_library_index链接绿（层守卫）＋e383e1e7六测试全绿（源码臂不污染）。
- [ ] **8.5 提交**：`feat(resolver): artifact arm for module knowability folding`。
- [ ] **8.6 T0**（ExprResolver宽面→保守Debug全量单测）。

### Task 9: 跨包重复成员诊断＋不可读包报告

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp`（发现扫描命中清单>1→诊断；不可读清单→not-found附注）
- Modify: `src/compiler/builder/ModuleRegistry.cpp:73-78`（ModuleNotFoundText附注形态或调用侧拼Note行——实现取更简者）
- Test: test_module_import诊断用例

**Interfaces:**
- Consumes: Task 4命中清单（`std::vector<命中>`：导入名→(包路径…)——**同目录辖域**，跨目录重复不进清单（D2目录序取先，评审三轮发现2））与不可读清单（ArtifactPackageCache成员）。
- Produces: 诊断一`Duplicate package 'tp.sub': 'dir/old.npkg' and 'dir/tp.npkg'.`（编译期硬错，点名两个包文件）；诊断二not-found报告附注`Note: package 'dir/x.npkg' could not be opened.`（门期臂与发现相位发射臂两处同口径——镜像运行期NoteTargetNotFound:220-243）。

**Steps:**
- [ ] **9.1 写失败测试**：(a) 两包成员表都含`tp.sub`→点名两包（现状RED＝静默取先）；(b) 目录含Open失败的坏包＋导入名无着落→报告带Note行（现状RED＝无附注）。
- [ ] **9.2 确认RED**。
- [ ] **9.3 实现**。
- [ ] **9.4 确认GREEN**。
- [ ] **9.5 提交**：`feat(imports): duplicate-member and unreadable-package diagnostics`。
- [ ] **9.6 T0**。

### Task 10: 运行期DiscoverClosure补staticSlotImports第五表

**Files:**
- Modify: `src/vm/NcuLoader.cpp:153-184`（DiscoverClosure四表:168-171扩第五表staticSlotImports——CompiledModule.h:419既有字段）
- Test: `tests/test_vm/`closure用例

**Interfaces:**
- Consumes: `CompiledModule::staticSlotImports`（include/nlang/vm/CompiledModule.h:419）。
- Produces: 运行期闭包walk五表与编译侧ImageImportTargets（ModuleBuilderImportCycles.cpp:91）同口径——「静态槽引用恒伴随classImports」的未成文不变量不再需要。

**Steps:**
- [ ] **10.1 写失败测试**：构造仅经staticSlotImports引用依赖单元的模块（探针钉真实可达形态；若实测定稿「不可单独出现」，改用合成wire模块＋DiscoverClosure直接断言五表walk——tests/test_vm/test_ncu_package.cpp合成先例）→闭包装载解析成功（现状RED＝缺边）。
- [ ] **10.2 确认RED**。
- [ ] **10.3 实现**：一行表walk＋既有序。
- [ ] **10.4 确认GREEN**＋既有closure族全绿。
- [ ] **10.5 提交**：`fix(vm): walk staticSlotImports in the runtime closure discovery`。
- [ ] **10.6 T0**。

### Task 11: e2e语料收口（双形态序与对拍）

**Files:**
- Create: `tests/e2e/`包产物语料族（夹具：预编译.npkg包构建脚本或复用gate-project harness形态；族注册tests/CMakeLists.txt＋tools/test_families.txt）
- Test: 本身即测试

**Interfaces:**
- Consumes: Task 4-10全部行为。
- Produces: §五e2e验收面的**工具链级语料**（评审M6裁决：ncc真实编译＋nvm真实执行——一个消费预编译.npkg的.n工程端到端，覆盖多成员单段/点分子模块/传递闭包/通配/双形态序/双形态对拍六面；spec§五「e2e」清单中编译期行为族已由Task 4-7的test_compiler族以gate-project harness承载（runGateProject真实编译＋运行退出码断言，满足「真实测试而非mock」），两级合计清偿验收面）——D2双形态序（同库陈旧.npkg前目录＋新源码后目录→源码赢、产物不遮蔽——评审关注点2）；源码/产物双形态同库对拍（行为与诊断一致）；产物内部目录序钉（两包异目录取先）。

**Steps:**
- [ ] **11.1 写失败测试**：双形态序用例——v1产物（旧行为）＋v2源码并存，断言v2行为（现状：导入解析源码臂已先行＝现状绿，本条是**回归钉**——钉住新产物臂不得反超；语料必须真实落位防将来倒序）；对拍用例——同一库源码形态与产物形态各跑一遍，输出一致。
- [ ] **11.2 实现语料与注册**（族名建议`pkg_artifact_imports`；tools/test_families.txt的src规则按夹具布局登记；**夹具.npkg一律放语料子目录**——评审L13：tests/e2e根下任何*.npkg触发run_e2e_tests.py:301-308的stray-shadow守卫exit(2)）。
- [ ] **11.3 全绿**＋T0。
- [ ] **11.4 提交**：`test(e2e): package artifact import corpus with dual-form ordering pins`。

### Task 12: 手册语义改写（导入行为面）

**Files:**
- Modify: `docs/user_manual/{en,zh}/specification/03-program-structure/packages.md`（§3.9.2改写：点分路径＝源码或产物任一可得即可导入，消除与developing-libraries.md§4矛盾；§3.9.4标准库包清单按领域重列13包；「包」语义统一——单元＝模块、包＝容器；已知限制核对——「点分路径无源码即编译期错误」限制解除）
- Modify: `docs/user_manual/{en,zh}/getting-started/install-layout.md:13/15`（评审二轮发现4：「十四个标准包」语义行正是spec§五「包语义统一」亲点例证——单元→包重列、stdlib目录行改13领域包；Task 3只机械改该页其余字面量，此行整行归本任务）
- Modify: `docs/user_manual/{en,zh}/vm-architecture/module-serialization.md`、`cli-tools/ncc.md`（包清单口径统一到D4清单：现状3成员/5成员/14单元三处不一致，按交付时点树内成员数核准）
- Test: 手册页级style＋snippets＋manual_sync流程

**Interfaces:**
- Consumes: Task 4-7落地的行为（语义描述与其一致）。
- Produces: 手册真源两语言成对的语义改写；本批含spec§五「包语义随拆包统一」全部页。

**Steps:**
- [ ] **12.1 预检**：`tools/manual_sync_check.py`判定Task 4-11各提交的用户可见面（语言面/诊断文案两类为主），全部体现在本批编辑。
- [ ] **12.2 编辑**：packages.md§3.9.2/§3.9.4/包语义/已知限制＋两页口径统一；中英成对、全称不折行。
- [ ] **12.3 验证**：页级style＋snippets审计；五腿docs检查留收口批复核。
- [ ] **12.4 提交**：`docs(manual): rewrite package import semantics for artifact consumption`；`--update`刷新基线核对no drift。
- [ ] **12.5 T0**（评审L12：纯文档批——影响映射无命中则豁免跑腿，与Task 3.5同形态）。

### Task 13: 容器格式1.1＋nproj native成员＋打包面

**Files:**
- Modify: `include/nlang/vm/NcuPackage.h:35-39`（NcuMember增`MemberKind kind`枚举Unit|Native，默认Unit）＋`:24-27`核对
- Modify: `src/vm/NcuPackage.cpp:19-21`（`NPKG_FORMAT_MINOR 0→1`）、`:91-117`（BuildNpkgHeader写版本1.1）、`:121-135`（BuildNpkgTable写种类字节）、`:183-227`（ParseNpkgHeader版本门：新读取器minor<=1；1.1包遇旧读取器（minor<=0门）拒绝——报错具名版本，§六守卫）、`:272-314`（ParseMemberTable读种类）
- Modify: `include/nlang/vm/NcuPackage.h:55-76`（增表级打开`OpenTable`——只解析header＋成员表、成员字节按需取（ExtractMember按表内偏移读）——评审L10：现状Open全文件贪婪载入且cache常驻，1.1包带多MB DLL时发现逐名开全部包＝全文件读＋常驻内存；表级打开让编译期只付表IO，发现扫描/折叠查询/通配扫描切到表级，装载期镜像与运行期闭包沿用全量Open；ArtifactPackageCache随同切表级驻留）
- Modify: `src/tools/ncc/ProjectFile.h:24-28`（`std::vector<std::string> nativeMembers`＋`<Native path="…"/>`XML元素，ProjectFile.cpp Load解析）
- Modify: `include/nlang/compiler/BuildEnvironment.h:31`（BuildParams增`std::vector<std::string> m_nativeMembers`）＋ncc main.cpp组装位
- Modify: `src/compiler/ModuleBuilderUnits.cpp:209+`（WritePackageArtifact追加native成员：键＝文件名、bytes＝打包时读取、kind=Native、校验和自动）
- Test: `tests/test_vm/test_ncu_package.cpp`（round.npkg族旁增1.1往返与版本拒绝用例）

**Interfaces:**
- Consumes: 无（纯增量）。
- Produces: 容器1.1读写（旧包1.0全兼容读取）；nproj生产面native成员；编译期ncc对native成员只在成员表可见（发现扫描排除native成员——Task 4扫描位点的kind过滤在此任务补上）。

**Steps:**
- [ ] **13.1 写失败测试**：test_ncu_package——(a) 1.1含native成员往返（MemberPaths含`nlang_net.dll`键、kind往返）；(b) 版本上限回归钉：手写minor=2的包→新读取器拒绝且报错具名版本（评审L8：既有1.0门minor<=0今天同样拒minor=2，此条**非RED来源**——钉的是门存在、防1.1升级时被意外放宽；「旧二进制拒绝1.1包」属部署面性质，由NPKG_FORMAT_MINOR单常量（NcuPackage.cpp:19-20读写共用）与不向下兼容原则承载）；(c) 1.0包在新读取器下全绿（既有用例即回归）。评审关注点5钉(b)的守卫存在性。
- [ ] **13.2 确认RED**（(a)为RED来源；(b)回归钉应恒绿——它红即门被意外放宽，同算发现）。
- [ ] **13.3 实现**：格式读写＋nproj/BuildParams/打包面＋`OpenTable`表级打开与编译期消费面切换（发现扫描/折叠查询/通配扫描——评审L10）。
- [ ] **13.4 确认GREEN**＋既有包用例全绿。
- [ ] **13.5 提交**：`feat(package): container format 1.1 with native library members`。
- [ ] **13.6 T0**。

### Task 14: 运行期抽取缓存＋搜索目录接线

**Files:**
- Modify: `src/vm/NcuLoader.cpp`（OpenPackage:307-323开包时抽取native成员到缓存；缓存目录集随装载回执暴露）
- Modify: `src/vm/NcuLoader.h`（`NcuLoader::Result`增`std::vector<std::string> nativeCacheDirs`回执成员——评审二轮发现2：宿主全部走静态`NcuLoader::LoadClosure(path, opts)`（ncc main.cpp:169-170、nvm main.cpp:232形态），实例访问器无宿主可持；回执成员是六位点同形的自然载体）
- Modify: `src/vm/VmExecutorNativeLoader.cpp:50-56`（EnsureNativeLoader().AddSearchDir既有形态——AddSearchDir注入的既有接收面）
- Modify: **六处宿主接线位点**（评审二轮发现2：`AddNativeSearchDir`先于closure装载的同形缺陷全仓共6处——D5.2契约是宿主无关的，漏任何一处，经该面消费带native成员的包会在`EnsureLoaded`找不到缓存DLL）：`src/tools/ncc/main.cpp`直跑路径:165-170与批路径:476-480、`src/tools/nvm/main.cpp:228-232`、`src/tools/ndb/main.cpp:87-93`、`src/tools/nlang/ScriptRunner.h:243-246`、`src/embed/InterpreterImpl.cpp:273-276`（**公开嵌入API装载路径——一处覆盖embed_host与全部第三方嵌入者**）；每处同形一行钉在LoadClosure回执之后：`for (const auto& dir : loaded.nativeCacheDirs) executor.AddNativeSearchDir(dir);`（评审M4：既有注入位在装载之前，彼时缓存目录集尚未填充、同位注入只会拿到空表静默失败）
- Test: `tests/test_vm/`抽取缓存用例

**Interfaces:**
- Consumes: Task 13的成员种类。
- Produces: 缓存根`PackageCacheRoot()`＝`%LOCALAPPDATA%\nlang\pkg-cache`（Windows；解析收口单函数，Linux形态后补）；缓存目录键＝包文件绝对路径哈希；抽取＝临时名写入后原子改名（并发进程至多读到旧或新完整文件）；落地前核成员校验和、不符→覆盖重抽；`EnsureLoaded`（NativeLibraryLoader.cpp:187-229）零改动——抽取文件保留原名（`nlang_<ns>.dll`，ModuleFileName:51-59自然命中）、懒装载维持；宿主流＝closure装载回执（`loaded.nativeCacheDirs`）→AddNativeSearchDir（六位点同形，VmExecutorNativeLoader.cpp:56接收面形态）；embed面经InterpreterImpl.cpp:273位点覆盖（公开嵌入API路径），无native调用的嵌入程序零波及。

**Steps:**
- [ ] **14.1 写失败测试**：包带native成员→运行调用该native的nlang程序成功（现状RED＝包内DLL不可见）；校验和重抽——篡改缓存文件→重抽后行为正常；不加载维持——导入该包但无native调用的运行中DLL不进进程（fixture DLL带attach标记文件，断言标记不出现——D5.2「抽取随包打开、懒的是装载」）。
- [ ] **14.2 确认RED**。
- [ ] **14.3 实现**：抽取＋原子改名＋校验和＋六位点宿主接线（回执成员）。
- [ ] **14.4 确认GREEN**。
- [ ] **14.5 提交**：`feat(vm): extract native package members to a user cache directory`。
- [ ] **14.6 T0**。

### Task 15: native e2e＋手册28-npkg格式节

**Files:**
- Create: `tests/e2e/`native包语料（夹具：小fixture native DLL（attach标记型，CMake测试目标）打包进第三方npkg）
- Modify: `docs/user_manual/{en,zh}/specification/formats/28-npkg.md`（容器格式1.1成员种类字段——中英成对）
- Test: e2e语料＋手册页级审计

**Interfaces:**
- Consumes: Task 13/14。
- Produces: §五native e2e验收面语料（加载运行/不加载维持/校验和重抽的e2e腿）；手册格式章1.1节。

**Steps:**
- [ ] **15.1 写失败测试**：e2e——第三方npkg带native成员，nlang程序经缓存目录装载调用（现状RED）；不加载维持e2e（评审关注点之外的D5.2钉）。
- [ ] **15.2 实现语料与注册**（族并入pkg_artifact_imports或独立`pkg_native`——实现取更简者；夹具.npkg放语料子目录——评审L13同Task 11.2）。
- [ ] **15.3 手册**：28-npkg.md 1.1节（manual_sync判定本批提交面）；页级style＋snippets审计。
- [ ] **15.4 全绿**＋T0。
- [ ] **15.5 提交**：`test(e2e): native member loading through the package cache`（手册同批或紧随提交，`--update`刷基线）。

### Task 16: 工程收口

**Files:**
- Modify: `CHANGELOG.md`（F1条目——用户可见面按五类清点：语言面导入语义、标准库包布局、诊断新族两条、容器格式1.1、native分发能力）
- Test: 收口门禁套件

**Interfaces:**
- Consumes: 全部前置任务。
- Produces: F1批收口。

**Steps:**
- [ ] **16.1 CHANGELOG条目**（中英成对核对根部文档规则）。
- [ ] **16.1b roadmap登记核对**（评审M5：spec§五工程面的「roadmap v0.13.12条目登记F1（含D5递延锚）」已随bfc0ce02入库，非本批新增——本步核对其在位、无递延锚孤儿；「实施计划路径引用」补写递延到版本收口roadmap合并批（评审二轮发现7：该文件带另一会话在途行，行级隔离暂存虽可行（bfc0ce02先例）但此处无独立必要——登记句已指spec，计划经spec可达））。
- [ ] **16.2 收口门禁**：T1全腿（Debug全量单测＋全量e2e＋**IDE树全量ctest**（编译器/导入/预置包变更必须——含Qt族）＋ASan单测＋ASan内存风险e2e子集）；ASan全量e2e与Release腿随0.13.12版本收口（与web线合流，双线roadmap合并先行——本计划不单独跑T2）。任一门禁红＝全批作废重扫。
- [ ] **16.3 慢测试与手册五腿**随16.2批（docs腿PYTHONPATH配方照nlang-docs-gate-recipe记忆条目）。
- [ ] **16.4 提交**：`docs(changelog): record package artifact consumption (F1) for 0.13.12`。
- [ ] **16.5 版本协调说明**：VERSION提升与0.13.12发布门禁由版本收口批统一执行（web＋F1同版合流；本计划交付到T1全绿为止）。

---

## 任务间接口总账（执行前预检用）

- Task 4是唯一大接口生产者：`PendingArtifactLoad`（含extIdx）/`m_pendingArtifacts`（按名去重）/`ArtifactPackageCache`（BuildEnvironment shared_ptr opaque成员）/物化衔接点（LoadImports入口、extIdx回写）/RegisterExternalStubs改按extIdx定位既有条目（其AddExternalModule退役）——Task 5/6/7/8/9全部消费，预检行：各消费任务声明与Task 4的Produces逐字段核对（name语义＝成员模块路径非导入名——两者在直文件子臂重合、在成员表子臂恒等）。
- Task 6把`ImageImportTargets`提升到`src/compiler/ModuleBuilderImportCycles.h`共享头（五表单一真源）——发现相位与循环检查两侧同一收集器；Task 10运行期第五表与编译侧同口径由此收口。
- Task 13的`MemberKind`与`OpenTable`被Task 4扫描位点（kind过滤、表级打开）与Task 7通配集排除共同消费——Task 13后必须回补Task 4扫描的native排除与编译期表级打开切换（13.3已含）。
- 手册两批（Task 3机械/Task 12语义/Task 15格式节）各自走manual_sync判定＋`--update`，不攒批。
- 提交纪律：全部逐文件点名；另一会话在途文件（tests/gate_time_reference.txt、docs/roadmap.md的v0.13.13行、docs/dev/generic_signature_design.md）绝不暂存。
