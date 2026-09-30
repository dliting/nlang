# 阶段 5「包名与类型身份」实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让每个源文件的包名由路径唯一决定，并让 VM 的类型表与函数表用同一个限定名键，
使 `pkg.Type` / `pkg.fn()` 在编译期与运行期都指向同一个身份；同时删掉 `namespace` 关键字。

**Architecture:** 包名取自编译器已有的所有者侧表（`ModuleRegistry::m_ownerOf` ＋
`ModulePathOf`），**不新建 AST 容器**；一条名字缝 `QualifiedName(const SnField&)` 同时被
解析器（编译器侧）与产码器（VM 侧）调用，因此源码里写的、表里存的、报错里显示的永远是
同一个串。语法面保持「点链在表达式里、归约时才定身份」的既有形状（实测：语句头的专用
带点范畴会抢走成员访问的 `'.'`），只把无检查的 `static_cast` 换成带检查的展开。

**Tech Stack:** C++17 ＋ CMake（静态库默认）＋ flex/bison 3.8.2 前端 ＋ 自研字节码 VM ＋
Qt5 nide ＋ 手写 `.nmod` 序列化（小端，`NLANGMOD` magic）。

**Spec:** `docs/dev/phase5_design.md`（R1～R5、D1～D13、§0.3 更正、`temp/probe/` 冲突实测）。
共同模型与阶段 6/7 边界见 `docs/dev/phases_567_design.md`。

## Global Constraints

- 行号基线 `d7ca710`（分支 `dev`）。工作树是 git worktree，所有命令在 `E:/cases/nlang/dev` 下跑。
- **构建**：`cmake --build build-dev --config Release -j 8`。**测试必须串行**：
  `ctest --test-dir build-dev/tests -C Release`（不加 `-j`）。基线 **63/63**。
  本计划**只在 Task 4 新增一条 ctest 条目**（`ndisasm_qualified_func_name`，理由与代码见
  Task 4 Step 8c——它是 D13 那一面唯一的门），其余任务的用例一律扩进已有测试文件。
  所以门限值随任务推进是：**Task 1～3 ＝ 63/63**（新条目还没出生），
  **Task 4 起＝64/64**（Task 4/5/6/7 的每一步；Task 8 只跑文档门，不跑 ctest）。
  把 64 写进 Task 1 或把 63 写进
  Task 5 都是错；这一步的口径在轮 2 就是因为自相矛盾被抓过（见 Task 6 Step 5 的引文）。
- **`ctest -R` 的三条语义陷阱**（轮 7 实测，本计划凡用 `-R` 的地方都按这三条写期望值）：
  ① **名字打错不算失败**：`ctest --test-dir build-dev/tests -C Release -R "definitely_no_such_test_xyz"`
  ⇒ 打印 `No tests were found!!!`、**退出码 0**。所以任何 `-R` 门都必须**先数选中的条数**：
  `ctest … -N -R "<同一条 pattern>"` 的列出条数要等于期望（例如 Task 4 Step 13 是 **2**）。
  ② **`DEPENDS` 在 `-R` 子集里不强制跑依赖**：实测 `ctest -R "proj_same_dir_run"` 用
  0.02 秒 **Passed**，而它的依赖 `proj_same_dir_compile` 根本不在选中集合里——它读的是磁盘上
  上一次构建留下的 `.nmod`。⇒ 靠 `DEPENDS` 排序的门，**必须把两条名字一起放进 `-R`**，
  并且**不能**把「依赖没生效」的症状写成「显示 Not Run」（那种状态在 `-R` 里出不来）。
  ③ 判定语只看 `Tests passed|failed out of N` 里的 **N**：`out of 1` 而期望 2 ⇒ 新条目没注册，
  这不是「门绿了」。**失败侧证据的句子必须写明 N**，光写「Failed」不够。
  ④ 引用 ctest 条目名时用**真名**：本仓库是 `stdlib_tests`、`native_abi_tests`／
  `native_loader_tests`／`native_host_tests`、`library_source_tests`……**没有** `test_stdlib`／
  `test_native_*` 这种名字（那是文件名，不是条目名）。
- **手写 `main()` 的测试文件里，「加了用例忘了注册」是静默的**（轮 7）。本计划的用例大多扩进
  「一条 ctest 条目＝一个 exe、`main()` 里手工串」的文件。`d7ca710` 实测的**形状表**
  （defs＝顶格定义数，bare＝`main()` 里 `TestX();` 形的调用数；两列相等的那些才可套对账）：

  | 文件 | defs | bare | 文件 | defs | bare |
  |---|---|---|---|---|---|
  | `test_vm/test_debugger.cpp` | 51 | 51 | `test_vm/test_native_abi.cpp` | 9 | 9 |
  | `test_vm/test_stdlib.cpp` | 19 | 19 | `test_langservice/test_symbol_index.cpp` | 9 | 9 |
  | `test_vm/test_library_source.cpp` | 11 | 11 | `test_vm/test_strings.cpp` | 7 | 7 |
  | `test_vm/test_library_search_path.cpp` | 5 | 5 | `test_vm/test_native_host.cpp` | 3 | 3 |
  | `test_compiler/test_array_flags.cpp` | 4 | 4 | `test_vm/test_thirdparty.cpp` | 2 | 2 |
  | `test_compiler/test_ast_containment.cpp` | 4 | 4 | `test_compiler/test_library_index.cpp` | 2 | 2 |
  | `test_compiler/test_array_token.cpp` | 3 | 3 | `test_compiler/test_scanner.cpp` | 3 | 3 |
  | **`test_vm/test_native_loader.cpp`** | **5** | **0** | **`test_vm/test_vm.cpp`** | **5** | **4** |

  最后两行是**异形注册**，套对账会把它们全报成漏注册：`test_native_loader.cpp` 走
  `run("LoadAndCall", &TestLoadAndCall, fixtureDir);`（`main()` 里的 lambda），
  `test_vm.cpp` 有一条包在 `try { … }`（`:213`）。往这两个文件加用例时**逐眼看 `main()`**，
  没有任何 grep 可替。其余 14 个文件，动到它们的任务收尾时跑对账（期望：**零行输出**）：
  ```bash
  for f in $(ls tests/test_*/*.cpp); do
    grep -qE 'QTEST_[A-Z_]*MAIN' "$f" && continue   #QtTest 由宏注册；异形注册的两个手动跳过
    comm -23 \
      <(grep -oE '^(static )?(void|bool|int) (Test|test)[A-Za-z0-9_]*\(' "$f" \
         | sed -E 's/.*[ ]((Test|test)[A-Za-z0-9_]*)\(/\1/' | sort -u) \
      <(grep -oE '^[[:space:]]+(Test|test)[A-Za-z0-9_]*\(\);' "$f" \
         | sed -E 's/[[:space:]]*((Test|test)[A-Za-z0-9_]*)\(\);/\1/' | sort -u) \
      | sed "s|^|UNREGISTERED $f: |";
  done
  ```
  **三个坑**（坑③ 是轮 7 审阅者的命令里我实测出来的）：① `test_import_parse.cpp`／
  `test_module_import.cpp`／`test_array_property.cpp`／`tests/test_nide/*` 是 QtTest
  （`private slots:` ＋ 宏自动注册；本仓库 QtTest 文件共 **15** 个：`QTEST_MAIN(` **5** 个、
  `QTEST_GUILESS_MAIN(` **10** 个，互不重叠——轮 10 按宏调用级 `grep -rlE 'QTEST[A-Z_]*MAIN\('`
  实测；
  槽名带缩进、模式本就不匹配，但 `continue` 仍要留着——它防的是「以后有人把槽写成顶格」；
  ② 排除宏时写 `QTEST_[A-Z_]*MAIN`，**不能**写 `grep -q QTEST_MAIN`：
  `QTEST_GUILESS_MAIN` 里不含 `QTEST_MAIN` 这个子串，写错等于没排除；
  ③ 两段 `sed` 不能省：直接把 `grep -oE '(Test|test)[A-Za-z0-9_]*$'` 接在第一个 grep 后面，
  两端**都是空集**（第一个 grep 的输出以 `(` 结尾，`$` 锚永不相合），对账于是在任何树上
  都报告「没问题」。审阅者给的命令就是这一形——他报的 11/11、51/51 等数是对的、命令是空的；
  我换成 `sed` 版重跑，才看到它把 `test_native_loader.cpp` 的 5 条全报成 UNREGISTERED，
  从而定出上面那张异形注册表。**结论：这条对账只覆盖「裸调用」那一形，别当全知门用。**
- **e2e runner 有一条不计失败的 SKIP**（轮 7）：`tests/e2e/run_e2e_tests.py:161-168` 对
  「manifest 里有名字、磁盘上没有对应 `.n`／目录」的条目，当 `out_dir == SCRIPT_DIR`
  （＝主 manifest 这一轮，`:47` 写死）时打印 `SKIP <name> (file missing)` 并 `continue`，
  **既不 `failed += 1` 也不影响退出码**。本阶段 Task 1/4/5/6 都要改名或新增 manifest 行，
  拼错一条就是少跑一条而读数照旧。⇒ 每个 e2e 门在后面加
  `| tee /tmp/e2e.log; grep -c '^SKIP' /tmp/e2e.log`，期望 **0**（`d7ca710` 实测：按
  manifest 逐名查存在性＝0 条缺失）。
- **工作树「干净」的门要写基线**（轮 7 实测，轮 10 重建时补第二条）：`git status --porcelain`
  在本工作树**恒有两行**：`?? main.n`（仓库根的游离源文件）与 `?? .zcodeignore`（本地 agent
  配置）——`.gitignore` 都没盖它们，不属于本阶段，**永不 add**。全篇
  「`git status --porcelain` 期望：空」的门因此永远红，看久了就被 eyeball 掉，真的漏文件反而藏在
  这条噪声里。统一写成：**除 `?? main.n`、`?? .zcodeignore` 外为空**，或直接用带路径限定的形式
  `git status --porcelain -- src tests tools docs stdlib examples`（期望空）。
- **文档管线门不在这 63 条里**——轮 7 把「为什么不在」查实了，因为这条正是门限曲线的地基：
  `nlang_docs_pytest` 确实是 `tests/CMakeLists.txt:1050-1065` 的一条 `add_test`，但它整块包在
  `if(NLANG_BUILD_DOCS)` 里，而本工作树的 `build-dev/CMakeCache.txt:202` 写的是
  `NLANG_BUILD_DOCS:BOOL=OFF` ⇒ 探测根本不跑、条目不注册 ⇒ `ctest -N`＝63（实测）。
  **所以本计划的每一个 63/64 都默认 `NLANG_BUILD_DOCS=OFF`**。开工前先跑
  `grep NLANG_BUILD_DOCS build-dev/CMakeCache.txt`：若为 `ON`，基线是 64，本节的曲线
  与 Task 1～7 的每处期望值都各自 **＋1**（不是「多了一条失败」），必须先改口径再动手。
  它必须单独跑，且必须带两个二进制（漏了会静默跳 5 条，读作 "62 passed, 5 skipped"）：
  ```bash
  PYTHONPATH=tools/nlang-docs/src \
  NLANG_NCC=$PWD/build-dev/tests/Release/ncc.exe \
  NLANG_NVM=$PWD/build-dev/tests/Release/nvm.exe \
  D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests -q
  ```
  期望 **67 passed**。Python 守卫脚本用 `D:/dev/miniconda3/python.exe`（裸 `python` 是
  WindowsApps stub，会 `Permission denied`，那不是失败）。
- **`source_size_guard`**（轮 7 重写：原稿只写了函数一条，而**先撞的是文件那条**；
  原稿点名的 `Register.cpp` 根本不会撞，`nlang.y` 更不在扫描范围内）：
  `tools/source_size_guard/check_source_size.py:35-36` 定死 **每文件 ≤500 行、每函数 ≤50 行**，
  扫描面是 `scanned_files()`（`:46-55`）＝ `src/`＋`include/` 的 `*.cpp/*.hpp/*.h`
  （**`.y`／`.l`／`.md` 不在内**）。本计划要动的文件里，`d7ca710` 的余量是：
  | 文件 | 现状 | 余量 | 谁在动它 |
  |---|---|---|---|
  | `src/compiler/SnExpressions.cpp` | **500／500 行** | **0** | **本计划没有任何一步动它**（轮 9 纠正：原稿写「Task 1 Step 5、Task 2 Step 3、Task 5」三条全错——Step 5 改的是 `include/nlang/compiler/SnExpressions.h`，那是 `allowlist.json` 第 1 条的**整文件豁免**，加行免费；而 `grep -n SnExpressions\.cpp docs/dev/phase5_plan.md` 在改之前只命中这张表自己这一行，`Task 5` 退役的 `SnNamespace` 实际住在 `src/compiler/SnMisc.cpp`）。列它只为了说明：**这文件已经顶格，谁将来想往里加行都得先拆**，本阶段不占用它 |
  | `src/compiler/SnMisc.cpp` | 255 行 | 充裕 | Task 5 从这里面删 `SnNamespace::` 一族（`MergeFrom`／`TestMemberAdding`／`AllowMember`／`Accept`…）——**只减不增**，方向安全 |
  | `src/compiler/builder/ExprResolverTypes.cpp` | 498 行 | **2** | Task 1 Step 6、Task 2 Step 4、Task 5 Step 6b、Task 6 Step 1 |
  | └ 其中 `GetGenericClassDecl`（`:103`） | span **50／50** | **0** | 前置事实「各加一行注释」若落在这里就越界 |
  | `src/vm/backend/Import.cpp` └ `MergeImportedTypeTables`（`:229`） | span 47／50 | 3 | Task 4 Step 7 改 `:236-263`（在它内部） |
  | `src/vm/backend/VmBackend.cpp` | 490 行 | 10 | Task 4 Steps 3/5/6/8 |
  | `src/vm/backend/Register.cpp` | 278 行，最大函数 27 | 充裕 | Task 4（原稿点名它是错的） |
  ⇒ 动到前两行的文件时**要么净零行（改一行删一行），要么在本任务里拆分**；
  走 `tools/source_size_guard/allowlist.json` 逃生门也可以，但**那条 JSON 必须进同一提交的
  `git add`**（Task 1/2/4/5 的 add 清单里原本都没有它，轮 7 已补）。
  这条门的现状是绿的（`D:/dev/miniconda3/python.exe tools/source_size_guard/check_source_size.py`
  ⇒ `source-size guard: clean`，退出码 0），所以它会**在改坏的那一次立刻红**，不用另建门。
- **禁止**：`git stash`（本工作树 `core.autocrlf=true` ＋ git-lfs 过滤器，round-trip 会改写行尾）；
  push；提交 `AGENT.md`/`CLAUDE.md`、`build-dev/`、`temp/`；改 `master`。
- **公开文字**（docs／`--help`／诊断文案）零旧引擎痕迹。
- **不留兼容层**：`.nmod` 格式 floor 与 ceiling 同抬，不做旧格式读；不给裸名回落；不加别名垫片。
- 测试一律**真实编译＋真实执行**（`ModuleBuilder::Build` → `ModuleLoader::Load` →
  `VmExecutor::Execute`），不 mock VM 内部。
- 每一步完成后按 `verification-before-completion`：报出跑了哪条命令、回报什么数字，才可以说绿。

## 执行简报（轮 10 重建 ＋ 执行风险评估，2026-09-30，开工前先读）

- **`nlang.y` 的两处编辑有顺序**：先做 Step 7 的账本块替换（`:1218-1245`），**再**插入
  `%expect 14`（`:8` 附近）——先插 `%expect` 会把账本块整体下移一行，按行号脚本切割就会
  割错行（Files 头警告过的 `:1218-1246` 事故正是这个形状）。两改完成后复核：
  `grep -n "^NameExpr:" src/compiler/grammar/nlang.y` 仍**恰好 1 命中**。
- **Step 2 的 `-R` 门先数选中数**：`ctest --test-dir build-dev/tests -C Release -N -R
  "library_source_tests"` 必须列出 **1** 条再跑真门（ctest 陷阱①的一致性要求；Step 2 原文
  「这里不需要 `-N` 的选中数检查」作废——那是就 `qhead_*` 三条说的，对 `-R` 名字本身仍要数）。
- **Steps 3～7 是一个构建单元**：Step 4 的动作调用 Step 5 的 `MarkMalformed`、Step 6 读
  `IsMalformed`，中途单独构建必然编译红。迭代期用 `ncc build` 直探 qhead 输入（秒级），
  e2e runner 是分钟级全量跑，只留给 Step 2 红 Run 与 Step 8 终门。
- **生成器 mtime 陷阱**：文法改完后构建，若 bison 的 custom command 没触发（mtime 不比
  `generated/nlang.tab.*` 新），会链接旧解析器、门永远红且原因难找——构建输出里必须看到
  bison 重新生成那一行才算数。
- **`compileLog` 随本任务一起落地**，虽然它的调用方在 Task 4 Step 1b——不是可延后项。
- **基线状态噪声**：`git status --porcelain` 恒有 `?? main.n`、`?? .zcodeignore` 两行
  （Global Constraints 已记），都**永不 add**；对这两行之外的任何脏行都要查出原因。
- **bison 路径**：门禁直接跑的 `win_bison` 与构建用的是同一份（`build-dev/CMakeCache.txt`
  的 `BISON_EXE=D:/dev/win_flex_bison/win_bison.exe`，PATH 里的同名），测量即构建。

## 前置事实（已实测，执行者不必重做）

- 冲突基线（**Task 1 落地之前**的树上实测，当时还没有 `%expect`）：
  `win_bison -d -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y`
  → stderr 两行：`warning: 14 shift/reduce conflicts` ＋ `note: rerun with option '-Wcounterexamples'`，
  退出码 **0**，冲突 **0 reduce/reduce**
  （state 148/185/191 各 1、254 共 9、290 共 2）。
  `nlang.y:1218-1245` 的账本注释写「1 rr ＋ 12 sr」，是过期的（Task 1 Step 7 重写它）。
  **Task 1 之后这条命令变成静音**：`%expect 14` 在位时 bison 一个字都不打印，
  要拿数字得用 `--report=all` 读 `.output`（测量与门禁的两条写法见 Task 1 Step 7 第 3 条，
  全计划后续四处语法门统一照那一对写，轮 9 已把 Task 2/Task 5 的三处期望就地改过来的原因就是这个）。
- `win_bison`／`win_flex` 在 `PATH`（`/d/dev/win_flex_bison`），`build-dev` 每次构建都从
  `src/compiler/grammar/nlang.y` 重新生成 `src/compiler/generated/nlang.tab.*`。
- `b.y`（只删 `namespace` 产生式）＝ 14 sr，零代价；`a.y`（语句头加专用 `TypePath`）＝ 16 sr，
  新增两条都在 `'.'` 且抢走成员访问 → 已否决；`d.y`（`':'`／`as` 复用 `NameExpr` 引导的
  `QualifiedType`）＝ 16 sr，新增两条良性（无产生式期望类型名后跟 `.`）。
  这三组数当时是在 `temp/probe/` 的副本上实测的（副本不提交）。**引用出处是上面那条命令，
  不是探针文件**：谁要复核就在真树上做出同样的最小编辑再量一次，别去 `temp/probe/` 找。
- `ModuleBuilder::MergeTransUnits`（`src/compiler/ModuleBuilder.cpp:233-258`）在产码**之前**
  把每个 TU 的根成员打标签（`TagUnitMembers`，`:262-271`）后合入 root，`EraseUnitOwners`
  只清未存活的壳节点 → **root 上的成员在 `GenerateCodes` 时仍带 owner 标签**，这是
  `PackageOf` 能在 VM 侧用的前提。
- 内建类型／合成泛型实例（`GetGenericClassDecl`，`src/compiler/builder/ExprResolverTypes.cpp:103-152`）
  从不被打标签 → `OwnerOf` 返回 `ModuleRegistry::NO_OWNER`（`ModuleRegistry.h:61`）→
  `PackageOf` 空串 → 天然保持裸名，泛型擦除键 `"List"`/`"Dict"` 不需要特例。
- `nlang_vm` 看不到 `src/compiler/builder/`（PUBLIC 只有 `include/`，见
  `src/vm/CMakeLists.txt:55-61`；轮 6 实测 `:63` 已经是 `target_link_libraries`）；Task 4
  要加一条 PRIVATE include 目录，反向先例是 `src/compiler/CMakeLists.txt:99`。
- `ModuleLoader.cpp:67` 硬编码 `minorVer < 12`，`CompiledModule.h:10-12` 的「single source of
  truth」注释不成立；`tests/packaging/verify_package.py:51` 也写了 12。三处一起改。
  （`ModuleLoader.cpp:71` 的上界是 `kCurrentMinorVer` 常量驱动的，所以只需要改 `:67` 的下界；
  `verify_package.py` 没接任何 ctest／CI job，它红了不会挡门，仍要改。）
  **升版时写 12 的地方不止三处**（轮 2 实测）：`docs/user_manual/en/vm-architecture/module-serialization.md:15`
  与 `docs/user_manual/zh/vm-architecture/module-serialization.md:13` 都写 `uint16 minorVer = 12`，
  `src/vm/ModuleSaver.cpp:39` 还有一份重复的「single source」注释。这四处和 12→13 同批改，
  文档门（Task 8）会抓前两条，但同批改完更省事。
- **库成员今天不在 root 上**（轮 1 实测，它决定了任务边界）：`stdlib/io.n` 顶层唯一的成员就是
  `namespace io` 这个壳节点，`SnNamespace::MergeFrom`（`src/compiler/SnMisc.cpp:50-86`，
  `MAK_Add` 分支 `:61-64`）搬进 root 的是**壳本身**，`print` 仍在壳里。所以
  `CompiledInFunctions`（`ModuleRegistry.cpp:292-300`）与 `FindModuleType`（`:377-385`）今天必须
  `pRoot->FindField(path)` 下钻。⇒ **「只扫 root ＋ owner 过滤」的写法在 `namespace` 壳还在时
  必然找不到任何库符号**，容器判定的退役必须和去壳在同一次提交里（Task 5），不能提前（Task 3）。
  **行号口径**（轮 7）：这里两对行号是**函数内部的回落那一段**（回落句实测在 `:295` 与 `:380`），
  函数体整体是 `:280-311` 与 `:359-397`，Task 3／Task 5 用的是后者。Task 5 Step 3b 按
  **行段**打勾，两套数不点名就会被读成「有一处已经改过」。
- stdlib 三个 `.n` **不声明任何类型**（`enum`/`struct`/`class` 计数为 0），所以 D12「缝合提交里
  stdlib 键不动」只对函数成立；类型表在 Task 4 之后没有 stdlib 条目。
- 名字查表的全量清单（`grep -rn 'Find\(Struct\|Class\|Function\|Enum\)(' src/vm` ＝ **35 处**，
  轮 2 实测；原稿写 38 是把 `CompiledModule.h:307/314/321` 的三个**定义**也数进去了——
  `FindEnum` 在本仓库根本不存在，闭合命令里不要带它）。Task 4 的表按这三类逐行给：
  **注意别和 `test_library_index.cpp:47` 的 `size() == 38`（符号索引条数「io 5, math 25, fs 8」）
  混为一谈，那是另一个 38。**
  - **吃 AST 名，必须改 `KeyOf`**：`Register.cpp:122/126`（`RegisterArrayType`，函数体 `:118-126`）、
    `RegisterClass.cpp:113/117/161/201`、
    `TypeDesc.cpp:179/229`（`FindStruct(pType->Name())`／`FindClass(pClass->Name())`）、
    `EmitCall.cpp:114`（形参型 `pFormalType->Name()`）、`EmitExprCast.cpp:279`（`targetType->Name()`）、
    `EmitExprInitList.cpp:131/307/345`、`EmitExprNew.cpp:56`、`EmitStmtAssign.cpp:62/311`、
    `EmitStmtDecl.cpp:35`、`EmitStmtForeach.cpp:251`、`EmitStmtStore.cpp:149`、
    `EmitStmtSwitchTry.cpp:428`。
    **`Register.cpp:86/101` 不在这张表里**（轮 7 更正，原稿把它和 `:122/126` 混列成一行）：
    这两处吃的是 `typeNames[i]`（`:63` 收的 `fieldType->Name()`），而 `:63` 本身就是写入端、
    本任务改成 `KeyOf(*fieldType)` ⇒ 消费端**跟着限定、代码一行不动**；
    在这里套 `KeyOf(typeNames[i])` 连编译都过不去（那是 `std::string`，不是 `SnField*`）。
    归到下面「吃自己产码的串」那一类，但它是**同一 TU 内的表**，不是字节码。
  - **吃内建字面量，必须保持裸名**：`VmExecutor.cpp:26/32`（`FindClass("List")`／
    `FindClass("Dict")`）＋`:38-48` 的 `cacheExcClass` lambda（`Exception` 一族的六个名字，
    轮 6 实测：`:38` 是 lambda 体里的 `module.FindClass(name)`，**不是** `Object`——
    `Object` 的裸名比较在 `VmExecutorOpsObjects.cpp:326`）、
    `EmitExprInitList.cpp:178/232`（`List`/`Dict`）。
  - **吃自己产码时写进字节码的串，保持裸名**（表键限定后自动跟着限定，无需改代码，但要各加一行
    注释说明它是「写读同源」的）：`VmExecutorSer.h:301`、`IntrinsicsByteStream.cpp:208/256`、
    `IntrinsicsFileStream.cpp:220/270`、`Import.cpp:237/248`。
- **只数 `Find*(` 的清单不足以闭合**（轮 2 实测）：表键的**写入端**和两处**手工扫表**都不在那个
  grep 里。补一条命令，把写入／比较端也纳入账本（实测 18 行，逐行归入三类）：
  `grep -rn '\.name = \|\.name ==\|name != ' src/vm include/nlang/vm src/tools/ndisasm/main.cpp`
  必须逐条交代：`Register.cpp:47`、`RegisterClass.cpp:95/176`、`Register.cpp:272`
  （`cf.name = QualifiedFunctionName(func)`＝函数键唯一写入点）、`Import.cpp:374`、
  `BuiltinClasses.cpp:221/239`（内建，裸名）、`VmBackend.cpp:32`（模块名，不是成员键）、
  `EmitStmtSwitchTry.cpp:316`、`VmExecutorOpsCalls.cpp:309`（**方法按裸名分派**，D8 的依据）、
  `VmExecutorOpsObjects.cpp:326`、`VmExecutorDebug.cpp:98`、`DebugSessionController.cpp:152`、
  `ndisasm/main.cpp:146`（`-func <name>` 过滤＝用户可见拼写，D13 面）、`CompiledModule.h:309/316/323`
  （三个 finder 的函数体本身）、**`VmBackend.cpp:418`**（轮 4 补：`desc.name = name` 写的是
  `LocalDescriptor` 的**局部变量名**，不是三类表键之一 ⇒ 归「保持裸名」，但必须显式交代，
  否则 18 行对不上数）。这 18 行是这条 grep 在全仓的完整输出，逐行核对用：
  `grep -rn '\.name = \|\.name ==\|name != ' src/vm include/nlang/vm src/tools/ndisasm/main.cpp | nl`
  ⇒ 数到 18 且每行都在上面的交代里。
- **调试／回溯面吃的就是表键**（轮 1 实测，计划原稿漏了这一整面）：`VmExecutorDebug.cpp:76`
  `info.funcName = frame.func->name`、`DebugSessionController.cpp:145/152`（函数名断点按
  `func.name` 匹配）、`VmExecutor::FormatBacktrace`（`VmExecutor.cpp:115-132` 直接打
  `f.funcName`）。限定之后单文件／`main.n` 场景里入口键＝`main.main`，所以
  `tests/test_vm/test_debugger.cpp`、
  `tests/test_nide/test_debug_client.cpp:162/178`、`tests/test_nide/test_mainwindow.cpp:2284/2556`
  都会红。规则见 D13。
  **`test_debugger.cpp` 的完整名单轮 4 实测＝14 处**（原稿只列了 `FindFunction("main")` 6 处＋
  `hooks.target = "main"` 2 处，漏了 6 处）：`FindFunction("main")` `:858/:892/:929/:973/:1273/:1766`、
  `FindFunction("triple")` `:184`（多文件用例，键变 `mathutil.triple`）、`hooks.target` `:680`（`"inner"`
  ⇒ `dbg_step_semantics.inner`）＋`:756/:795`（`"main"`）、`AddFunctionBreakpoint("inner")`
  `:1432/:1518/:1520`、`AddFunctionBreakpoint("val")` `:1475`。闭合命令（跑它，行数必须是 14）：
  `grep -n 'FindFunction("\|AddFunctionBreakpoint("\|hooks.target = "' tests/test_vm/test_debugger.cpp | wc -l`
- 函数**同名跨模块是合法重载**，不是撞名：`SnMisc.cpp:88-115`（`FindOverloadBlocker` 只拦非函数
  同名，同参数个数的放行给 `DuplicateFieldChecker` 精判）。⇒ 重复键诊断**只加在类型表**，
  函数侧加了就会误杀 `default_overload_basic.n` 这类正例。
- 路径勘误（计划原稿写错）：`VmBackend.h` 在 `src/vm/`（不是 `src/vm/backend/`），且它**没有**
  `m_Env` 成员（只有 `SaveModule(BuildEnvironment&)` 形参）→ 查重诊断的日志通道必须显式注入；
  `TestNatives.h` 在 `src/vm/TestNatives.h`（`:61-64` 注册的正是裸名 `natAdd`/`natConst`/
  `natFAdd`/`natPing`）；`MachineFrontEnd.cpp` 在 `src/tools/ndb/`。
- `src/compiler/generated/nlang.tab.*` 是**构建期产物且被 gitignore**（`.gitignore:25`），
  只有 `FlexLexer.h` 入库 → 任何提交都**不要** `git add` 生成文件。`stdlib/*.n` 则是正常入库的
  （`git ls-files stdlib` ＝ 3）。
- 仓库根有 4 个 gitignore 的散装 `.nmod`（`prog.nmod`/`good_widen.nmod`/`sig_check.nmod`/
  `test_coerce.nmod`）＋ 未跟踪的 `main.n`。`.nmod` 全是 v12，格式升到 13 后会被拒读；它们不是
  任何测试的输入（`test_thirdparty.cpp:58` 还会主动清 `prog.nmod`），执行时**不要**去改或删它们。

---

### Task 1: 语法账本对齐 ＋ 限定类型链的类型检查

**Files:**
- Modify: `src/compiler/grammar/nlang.y:1218-1245`（账本注释整块——**轮 8 改界**：原稿写 `:1218-1246`，
  而实测 `:1246` 是 `NameExpr: IdentifierExpr { $$ = new SnNameExpr($1, @1); } ;` 这条**产生式**，
  照原稿整段替换会删掉语法本身。块的起点也不是 `:1229`：`:1218-1228` 讲的是
  「NameExpr 曾经也派生 MemberExpr／剩下那一条 rr 在 `<` 上」，与本任务的新数字直接矛盾
  （实测 **0 rr**），留着就是文法文件里自相矛盾的两段话。Step 7 的替换范围因此是 **1218-1245**），
  `:155-172`（`CollectQualifiedSegments`——注释 `:155-159`＋函数体 `:160-172` 两半，Step 3 的替换体
  自带新注释，只换 `:160-172` 会把旧注释留在头上）、`:1324-1332`（`HeadType: MemberExpr` 动作）
- Modify: `src/compiler/builder/ExprResolverTypes.cpp:332-340`（`Access(SnQualifiedTypeExpr&)` 的
  `segs.size() < 2` 分支。**轮 8 改界**：原稿写 `:327-340`，而 `:327` 是函数签名行、`:329-330`
  是 `IsResolved()` 早退，Step 6 的替换体从 `const auto &segs` 起——真正被换的是 `:332-340`
  共 **9 行**。本文件在 `d7ca710` 是 **498 行**、上限 500，**顶格不算越界但没有余量**，所以
  Step 6 的替换体必须做到**净零行**。**轮 10 更正**：Step 6 正文的替换体（轮 8 版）已经是
  **9 行＝净零行**；本条旧稿里「替换体 11 行 ⇒ 净 +2」的算术和「把 `:325-326` 压成一行」的
  备选**作废**——按 Step 6 正文执行，**不要碰 `:325-326`**。）
- Modify: `include/nlang/compiler/SnExpressions.h`（`SnQualifiedTypeExpr` 的「never reaches codegen」注释）
- Test: `tests/test_vm/test_library_source.cpp`（进程内只留对照正例）＋
  Create: `tests/e2e/qhead_call/{order.txt,alib.n,main.n}`、
  `tests/e2e/qhead_deep/{order.txt,alib.n,main.n}`、`tests/e2e/qhead_index.n`
  ＋ Modify: `tests/e2e/manifest.txt`
  （三条 `compile_error` 钉；前两条是目录型，形状照 `tests/e2e/native_crossmod/`，
  第三条是单文件型）

**Interfaces:**
- Consumes: 无（不改任何名字规则）。
- Produces: `static bool CollectQualifiedSegments(SnExpression* pExpr, std::vector<std::string>& out)`；
  测试侧的新 helper `runBuild(dir, logger, vrSources)` 与它的两个包装 `compileLog`／`compileDir`
  （**轮 8 补进 Produces**：原稿只列了前两条，而 Task 2 与 Task 4 的进程内用例直接吃
  `compileLog(dir, {"x/main.n", "y/main.n"})` 这一形状）；
  `SnQualifiedTypeExpr` 上的 `bool IsMalformed() const` ＋ `void MarkMalformed()`。
  **这两个 flag 的消费方只有本任务自己**（Step 4 的语法侧写、Step 6 的 resolver 侧读）——
  轮 8 把原稿的两处口径错一起改掉，轮 10 重建时再改成**结构式判定**（绝对行号会随稿子增删
  漂移，两次实测都撞上了）：① 原稿说 Task 2 的 `TypeName` 也消费它们——不成立，Task 2 全文
  没提过这个概念（Task 2 依赖的是 Step 5 那条 `TypeName` 产生式，不是 flag）；
  ② 原稿引的 grep 命中行号是旧稿的，不作数。**判定方法**：
  `grep -n IsMalformed docs/dev/phase5_plan.md` 的每个命中，看它落在哪个区——前言
  （Global Constraints／执行简报／前置事实）、本任务正文（Task 1 标题行 → Task 2 标题行）、
  Task 9 之后的自查／审核记录区，都算本任务自用或全计划元描述；
  落在 Task 2～8 的正文里才算有别的任务在消费——今天的答案是「没有」。
  **这不改变本任务要造这两个名字**：Step 6 的守卫要靠它把「语法拒了的点链」与
  「合法但只有一段」分开，两者共用同一条 `Malformed qualified type reference.` 诊断。

- [ ] **Step 1: 写失败的测试——非法点链必须是诊断，不是崩溃也不是静默**

**负例不进进程内测试，进 e2e 子进程门**（轮 4 实测的决定性纠正）。`CollectQualifiedSegments`
的盲下转型今天不是「抛个可捕的异常」，而是**未定义行为，同一份源文件重跑结果会变**：

```
build-dev/tests/Release/ncc.exe，输入 alib.twice(1).Box v; 连跑 5 次：
  run1 rc=1  Compiler internal error: bad allocation
  run2 rc=1  Compiler internal error: bad allocation
  run3 rc=1  （走完）Error: Module 'alib. 4' is not imported.   ← 段名字符串被读坏
  run4 rc=1  Compiler internal error: bad allocation
  run5 rc=1  （同上，文案里带脏字节）
另一形状 `q[0].x v;`（q 是 int 变量）实测 0xC0000005，shell 报 Segmentation fault、rc=139。
轮 5 复跑同一形状 3 次：run1 = 脏段名（`Module 'q.<乱码>' is not imported`），run2／run3 =
`Compiler internal error: bad allocation`。本轮没再复现到 AV，**但这不改变结论**——同一份
输入在同一次构建的二进制上给出三种出路，进程内断言无论钉哪一条都不成立。
```

进程内断言因此不可用：`bad_alloc` 那一路能被 `try/catch` 吞，AV 那一路会把
`test_library_source.exe` 整个打死（其余用例全部不算数），脏字符串那一路连断言对象都不稳定。
e2e runner 逐条用 `subprocess.run` 起 ncc（`tests/e2e/run_e2e_tests.py:211-215`），崩只影响那一条
用例；`compile_error <期望串>` 比对失败模块的 stderr（`:234-244`），三条 UB 出路全都不含目标文案
⇒ 今天必红、修好必绿、且不污染测试进程。

新增三条 e2e 用例（前两条**目录型**：`order.txt` 依赖在前、消费方在最后，runner 以
`-I <用例目录>` 逐个编译。形状照 `tests/e2e/native_crossmod/`，但**两处别照抄**——
轮 8 实测：① 它的 `order.txt` 写的是 `nativelib nativemain`，最后一名不叫 `main`；
runner 是按下标取最后一个模块跑的（`run_e2e_tests.py:270` 的 `modules[-1]`），
所以本用例写 `alib main` 成立，但要清楚**成立的是位置不是名字**。
② 它的 `nativelib.n` **没有** `namespace` 外壳（全文只有 `native int natConst();` ＋顶层
`int wrapped()`），所以它不是「带壳库源」的样板；本任务的 `alib.n` 要带壳，
样板是同仓库里的 `kLibSource`（`tests/test_vm/test_library_source.cpp:104-116`）。
原稿写「与 `nativelib.n` 同形」又写「仍带 `namespace alib { … }` 外壳」，两半各自对得上
别的文件、合起来互相矛盾，照着找样板的人会拿错样板）：

```
tests/e2e/qhead_call/    order.txt 内容：alib main
  alib.n （两条目录型用例共用同一份，逐字如下——内容就是 kLibSource 的展开，
          本阶段删壳之后它会变成裸声明，那时由 Task 5 改）：
           namespace alib {
           int twice(int x) {
             return x * 2;
           }
           }
  main.n ：import alib;
           int main() {
             alib.twice(1) v;      //调用落在类型头位置
             return 0;
           }
tests/e2e/qhead_deep/    order.txt 内容：alib main
  alib.n ：与 qhead_call/alib.n **逐字相同**（各留一份，别做成共享 fixture：
           两条用例的失败面不同，共享之后任何一次单边改动都会让另一条静默换语义）
  main.n ：import alib;
           int main() {
             alib.twice(1).Box v;  //链的更深处是调用：递归要在下一层报 false
             return 0;
           }
tests/e2e/qhead_index.n  （**轮 5 补的第三条，设计 §13 点名的下标形状**——原稿只有
                        调用形与「调用嵌在链里」两种，`a[0].x v;` 这一条没钉；
                        轮 8 曾记它「实测唯一打出 0xC0000005」，轮 9 审阅者同一形状 5 次
                        未重现 ⇒ 崩溃与否不是这条用例的判据，判据只有期望串那一条）
           int main() {
             int q;
             q[0].x v;             //下标落在类型头位置
             return 0;
           }
```

`tests/e2e/manifest.txt` 三行——期望串就是 `ExprResolverTypes.cpp:337-338` 今天已经发的那条
（实测原文 `Malformed qualified type reference.`，**轮 5 复核**：`sed -n '337,338p'` 给出的
就是 `m_Env.Log(CLL_Error, qtype.Location(), "Malformed qualified type reference.");`），
本任务复用文案、不加第二种诊断：

```
qhead_call compile_error Malformed qualified type reference
qhead_deep compile_error Malformed qualified type reference
qhead_index compile_error Malformed qualified type reference
```

第三条是**单文件形**（`tests/e2e/<name>.n`，不需要 `order.txt`）；`q[0].x` 的 `q` 是本地
`int` 变量，不涉及库，所以不用目录型。
**轮 8 订正这条的形状判据**：原稿写「runner 的 `:161-162` 先找 `<name>.n` 再找 `<name>/`」，
实测两种形状同时存在时**目录优先**——`:159-161` 是一个联合判断
（`if not isfile(test_file) and not isdir(test_dir)` 才算缺失，没有先后），
而真正的分岔在 `:174`（examples 清单里的目录型条目直接 FAIL）与 `:188`
（`if os.path.isdir(test_dir):` 走多模块路径），目录分支排在单文件路径**前面**。
对本任务的实际影响：`qhead_index` 只要别手滑建出 `tests/e2e/qhead_index/` 目录就没事，
而**万一建了出来，它会静默盖掉 `.n` 那份**、然后因缺 `order.txt` 报
`FAIL qhead_index (missing order.txt in test dir)`——真跑的时候如果看到这条 FAIL，
症状就是目录残留，不是语法问题。收口检查：`ls -d tests/e2e/qhead_index tests/e2e/qhead_index.n`
必须恰好一条存在。

> 放宽判据＝空转：只写 `compile_error` 不带期望串的话，今天的三条 UB 出路里有两条会被当成
> 「编译失败＝符合预期」放过。也不要为过门去改别的用例。

**轮 9 定层（后台审阅者 F5，实测过才写）**：这三条形状**在语法层全部通过**，今天就已经落到
解析端——它们那些乱七八糟的错误来自 `RejectUnimportedQualifiedType` 与「not a member」分支，
不是 bison。证据：把 Step 3＋Step 4 的替换体原样打进一份文法副本再跑 `win_bison`，
`--report=all` 量出来仍是 `sr=14`、`rr=0`（**这次复测做在 Task 1 之前、`%expect` 还没进序言的
树上，所以当时是直接读退出流的 `warning: 14 shift/reduce conflicts`；Step 7 落地之后同一件事
要用上面第 3 条那组测量命令，退出流已经静音**）。⇒ **本任务没有任何一处改产生式**，
Step 7 的账本注释改的只是「账」，而唯一的机械新增是 `%expect`。
由此两条实现期不必再查的事实：
①**修好后仍然安全**——未解析的限定类型节点带错误时，`ModuleBuilder::Build` 在
`ModuleBuilder.cpp:107-108` 的 `HasError()` 上先返回，产码看不到它；单参 `ncc build` 的
同类形状（`nosuch.Box b;`）实测只发一条干净错误、不产出 `.nmod`。
②**判据只有 `IsMalformed()` 那一半在真的干活**：`segs.size() < 2` 今天是不可达的
（现有动作无条件取 `segs.at(1)`，链至少两段才走到这里），但它是 `:332-340` **今天就有的**
守卫，本任务按「只加一个条件、不改文案」保留它——别把它当新增死码删掉，那会让
Step 4 的 malformed 分支变成唯一入口而少一道防线。

进程内这边只留**今天就是绿的对照正例**（钉「别把成员访问一并禁掉」），沿用文件里已有的
`scenarioDir`／`writeFiles`／`compileDir`：

```cpp
//Phase 5 audit I2 control: a member access in EXPRESSION position must stay
//legal. The Step 3 guard only rejects a non-dotted chain in TYPE-head
//position — if this case goes red, the guard widened too far.
//The two malformed-chain negatives are deliberately NOT here: they are
//undefined behaviour on current dev (blind static_cast in
//CollectQualifiedSegments, nlang.y:160-172) and are pinned as the e2e
//subprocess cases qhead_call / qhead_deep / qhead_index instead, so a crash can take out
//one e2e entry rather than this whole binary.
static void TestQualifiedTypeHeadKeepsLegalMemberAccess() {
    auto dir3 = scenarioDir("qhead_ok");
    writeFiles(dir3, { { "alib.n", kLibSource }, { "main.n",
        "import alib;\n"
        "int main() {\n"
        "  return alib.twice(1) - 2;\n"
        "}\n" } });
    CHECK(compileDir(dir3), "member access in expression stays legal");
}
```

`compileLog(dir)` 本步仍要加（Task 4 Step 1b 的四条文案断言吃它：`(1b)` 未导入、`(2)` 同包重名、
`(3b)` 歧义、`(3c)` 未命中）——`compileDir`
（`tests/test_vm/test_library_source.cpp:68-82`）把 `ListCompileLogger` 丢在函数作用域里，
测试拿不到文案。做法：照 `compileDir` 抄一份，遍历
`logger` 的 `cbegin()/cend()`（`include/nlang/compiler/Logger.h:97-119`，条目是 `CompileLogItem*`）
把消息拼成一个 `std::string` 返回；原 `compileDir` 改成调它并只看 `Build()` 的返回值
（一条日志通道、一份拼装，不留两个并行的编译入口）。

**轮 6 实测：真正的公共那段是 build，不是入口名**。今天的 `compileDir` 只 push 一份源文件
（`:70` `params.m_SourceFiles.push_back((dir / "main.n").string())`），而本计划有两处需要
别的形状：Task 4 Step 1b 的库单元（没有 `main.n`，入口是 `alib.n`）、Task 4 Step 8 的
两个项目 TU（`a.n`＋`b.n`，**必须一次 `Build()` 里两份源**，否则第二份根本不会被编译，
「两个 `main()`」这条分支永远走不到）。`BuildParams::m_SourceFiles` 本来就是 vector，
`ModuleBuilder.cpp:208` 逐份解析、`RegisterUnits`（`:138`）逐份注册 ⇒ **多 TU 编译今天就是
产品能力**（`ncc` 只在 `.nproj` 模式下喂多份：`src/tools/ncc/main.cpp:279-284`，所以测试
必须自己直接填 `m_SourceFiles`，走不到 CLI）。所以这一份 build helper 从第一步就带源列表：

```cpp
//The shared compile: every name in vrSources is one project TU, resolved under
//dir as the project root. Default is the single-file shape the existing tests
//use. m_sProjectDir = dir is behaviour-preserving for every flat scenario
//(DeriveModulePath, src/compiler/builder/ModuleRegistry.cpp:48-65: "main.n"
//relative to dir -> DotifyModulePath gives "main", and a library TU outside dir
//hits EscapesBaseDir and falls back to its stem exactly as today) but it is what
//makes NESTED fixtures expressible in-process: Task 4 Step 8's two-main case
//needs x/main.n + y/main.n to land in two different bare pools, and without a
//project root both stems degenerate to "main" (same pool -> the wrong
//diagnostic). Nested-directory projects for the gate live in
//test_module_import.cpp:91-121; this helper does not replace that one.
bool runBuild(const fs::path& dir, ListCompileLogger& logger,
    const std::vector<std::string>& vrSources = { "main.n" });
```
`compileDir(dir)`／`compileDir(dir, {"alib.n"})`／`compileLog(dir, vrSources)`／
`compileRun(dir, cap, vrSources)` 全部是它的一行包装；**不要**为「换个入口名」再抄一份
build，也不要造 `compileDirOnly`/三参 `compileRun` 这种文件里不存在的东西。

**helper 自己吞异常这一层保留**，但**理由换了**（轮 4 订正）：轮 3 写这条时以为链负例（当时两条，
轮 5 按设计 §13 补成三条）会进程内抛 `std::bad_alloc`；轮 4 实测它们是 UB（有时 `bad_alloc`、有时 AV、有时脏文案），
所以已挪去 e2e 子进程门。留下来的 catch **不为任何断言服务**，它的存在理由是
**今天的 `compileDir` 本来就有这个 catch**（`:80-81`
`try { return builder.Build(); } catch (const std::exception&) { return false; }`）——
拼装文案的函数是它的改写版，搬迁时把这层保护丢了才是回归。Step 1b 那四条文案断言
（`(1b)` 未导入、`(2)` 同包重名、`(3b)` 歧义、`(3c)` 未命中）今天走到的是**正常日志通道**，
不是 `throw`：

```cpp
//One build entry point for the whole file: fills params, runs Build(),
//and leaves the diagnostics in the caller's logger. Never a second copy.
bool runBuild(const fs::path& dir, ListCompileLogger& logger,
              const std::vector<std::string>& vrSources = { "main.n" }) {
    BuildParams params;
    for (const auto& s : vrSources) params.m_SourceFiles.push_back((dir / s).string());
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    return ModuleBuilder(params, logger).Build();
}

//Text view: joins every logged item. ListCompileLogger only exposes
//cbegin()/cend() and stores CompileLogItem* (Logger.h:97-119).
std::string compileLog(const fs::path& dir,
                       const std::vector<std::string>& vrSources = { "main.n" }) {
    ListCompileLogger logger;
    std::string out;
    try { runBuild(dir, logger, vrSources); }
    catch (const std::exception& e) {      //kept because today's compileDir
        out += " internal error: ";        //already has this catch (see above)
        out += e.what();
        return out;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out += (*it)->Message() + "\n";
    return out;
}

//Boolean view: today's call sites keep compiling (default argument).
bool compileDir(const fs::path& dir,
                const std::vector<std::string>& vrSources = { "main.n" }) {
    ListCompileLogger logger;
    try { return runBuild(dir, logger, vrSources); }
    catch (const std::exception&) { return false; }
}
```
**轮 8 把这一段整块重写**：原稿在这里点名了四个 helper，其中 `DrainLogger` 与
`compileAndCheck` **全篇没有定义**，`compileLog` 还给了两种互斥签名（一参
`const std::string&` 版与两参 `fs::path` 版），而同一步上面自己写的规矩正是
「不要造文件里不存在的东西」（`:488-489` 的原文是「不要为『换个入口名』再抄一份 build，
也不要造 `compileDirOnly`／三参 `compileRun` 这种文件里不存在的东西」——原稿自己就在这条上犯规）。
现在**只有三个名字**（`runBuild`／`compileLog`／`compileDir`），每个都有字面定义。
`compileRun` 走同一条路加第三个默认实参：今天它是 `:90-100` 的
`int compileRun(const fs::path& dir, CapturingIo& cap)`，改成
`int compileRun(const fs::path& dir, CapturingIo& cap,
                const std::vector<std::string>& vrSources = { "main.n" })`，
内部把 `compileDir(dir)` 换成 `runBuild(dir, logger, vrSources)`。
**轮 8 实测订正**：`compileDir` 定义在 `:68-82`，调用点 **5 处**
（`:91` 在 `compileRun` 内部、`:144`、`:153`、`:169`、`:429`）；`compileRun` 调用点 **7 处**
（`:133/:189/:245/:296/:339/:373/:407`）。加了默认实参之后这 12 处**一行都不用动**——
这才是「改签名不动调用点」的实际含义，别去逐处补 `{"main.n"}`。

`CHECK` 宏自己记账，不手写 `++g_pass`。用例必须注册进文件的 `main()` 调用串（与
`TestModuleTypeOwnerIsolation` 同一处）——**不注册的用例会编掉，门就是空转**。
本任务 ctest 条目数不变（进程内只有对照正例，算进既有 `library_source_tests`；e2e 三条走
`run_e2e_tests.py`，不是 ctest 用例）。

- [ ] **Step 2: 跑门确认失败**

```bash
cmake --build build-dev --config Release -j 8 --target ncc test_library_source
# 0) 注册对账（Global Constraints 的手写 main() 形状表把 test_library_source 记成
#    定义数＝注册数＝11；本任务往文件里加**函数**，函数加了不注册就是空转）
#    期望：零行输出。轮 9（后台审阅者 F2）：原稿这里说「零行 UNREGISTERED 输出」却
#    漏了 Global Constraints 那版的 `| sed "s|^|UNREGISTERED $f: |"` 尾巴——不带前缀时
#    命令输出的是裸函数名，按字面找「UNREGISTERED」的人会在一地红上读出「零行」当绿。
#    下面这版与 :66-71 逐字一致，别再手抄成两形。
for f in tests/test_vm/test_library_source.cpp; do
  comm -23 \
    <(grep -oE '^(static )?(void|bool|int) (Test|test)[A-Za-z0-9_]*\(' "$f" \
       | sed -E 's/.*[ ]((Test|test)[A-Za-z0-9_]*)\(/\1/' | sort -u) \
    <(grep -oE '^[[:space:]]+(Test|test)[A-Za-z0-9_]*\(\);' "$f" \
       | sed -E 's/[[:space:]]*((Test|test)[A-Za-z0-9_]*)\(\);/\1/' | sort -u) \
    | sed "s|^|UNREGISTERED $f: |";
done
# 1) 本任务不新增 ctest 条目（新用例并进既有的 `library_source_tests`），
#    所以这里不需要 `-N -R` 的选中数检查；但 `qhead_*` 三条也**不是 ctest 用例**
#    （`manifest.txt` 不在 ctest 里，Task 4 Step 11 实测），门在下面第 2 步。
ctest --test-dir build-dev/tests -C Release -R "library_source_tests"   # 期望：Passed（对照正例）
# 2) e2e 门（末尾的 SKIP 计数是 Global Constraints 定的规矩：漏一个文件名不会红，只会 SKIP）
D:/dev/miniconda3/python.exe tests/e2e/run_e2e_tests.py \
  build-dev/src/tools/ncc/Release/ncc.exe build-dev/src/tools/nvm/Release/nvm.exe \
  | tee /tmp/e2e_step2.log
grep -c '^SKIP' /tmp/e2e_step2.log      # 期望：0（有 SKIP 就说明某条新用例根本没被跑到）
# 3) 红 Run 之后清掉目录型用例留下的 .nmod（轮 9 提出、轮 10 改成逐文件写）
#    只有 alib.nmod 会被留下：order.txt 是 `alib main`（空格分词＝两个模块，
#    run_e2e_tests.py:196），main 才是失败的那一个，所以 main.nmod 从不存在。
#    原稿写 `rm -f tests/e2e/qhead_call/*.nmod` 通配两条目录——形状上没错，但执行者
#    照字面去找 `main.nmod` 会发现它不存在、于是怀疑整条用例没跑到。
ls tests/e2e/qhead_call/alib.nmod tests/e2e/qhead_deep/alib.nmod 2>/dev/null
rm -f tests/e2e/qhead_call/alib.nmod tests/e2e/qhead_deep/alib.nmod
```
**为什么清**：`run_e2e_tests.py` 只在**期望命中**的 compile_error 分支删中间产物（`:247-251`），
而**diagnostic mismatch 分支直接 `continue`（`:238-244`）不删**。本步正是走 mismatch，
而 `:211` 给 `order.txt` 里**每个**模块都指定了 `<name>.nmod` 当输出（`:214` 的 argv），
所以先编成功的 `alib.nmod` 会留在 `tests/e2e/qhead_call/`、`qhead_deep/` 里。
留着的代价不是「脏」（**轮 9 实测：`.gitignore:33` 有 `*.nmod`，`git check-ignore -v` 确认命中，
所以它不进 `git status --porcelain`，不会把提交前的树干净门搞红**——审阅者 F4 的这条前提不成立），
而是 `:221` 用 `os.path.isfile(out)` **代替退出码**判断某个模块编过了：一个**上一次运行留下的
`.nmod`** 会让「这次其实编译失败」读成「编过了」，于是循环继续、最终报
`expected compile_error but compiled ok`。**改文法／改解析这类迭代恰好会翻转单个模块的成败**，
所以每次红 Run 后删一次，成本一行、去掉一类假象。
期望：`qhead_call`／`qhead_deep`／`qhead_index` 三条 **FAIL（diagnostic mismatch）**，runner 贴出的
stderr 是今天实测到的某条 UB 出路。**轮 8 把出路面补全**（原稿只列了三种），**轮 9 把「哪几次」
的口算删掉**（后台审阅者独立复跑 5 次，④那一面一次都没重现，说明这一族出路**按进程、按堆
状态漂移，不可复现**；把次数写进计划只会让下一个人去核对计数而不是核对断言）：
①`Compiler internal error: bad allocation`；
②脏模块名 `Module 'alib.<乱码>' is not imported`／`Module 'q.<乱码>' is not imported`；
③**脏类型名 `Type '<乱码>' is not a member of namespace 'alib'`**（轮 8 复跑 `alib.twice(1) v;`
实测到，原稿没有这一条；轮 9 审阅者在 `qhead_call` 上再一次实测到）——这条最危险，因为它读起来
像一条正常的语义诊断，扫日志的人会当成门已经绿了；
④进程直接 AV（`0xC0000005`）：轮 8 在 `q[0].x v;` 上实测到过，轮 9 同一形状 5 次未重现。
**这条不是断言的一部分**，列它只是为了说明「这条 UB 有走到崩溃的边缘」。
**门的判据只有一条**：上面任何一面都**不含**期望串 `Malformed qualified type reference`
（轮 9 实测：`grep -rn 'Malformed qualified type reference' src include tests tools docs`
＝**仅 1 处**，`ExprResolverTypes.cpp:338`，所以红 Run 的任何输出里都不可能出现这个串，
不存在「碰巧匹配上」的假绿），所以三条必红；看到 ①②③ 中任意一面都算符合预期，
**不要**因为没看到某一面而怀疑用例放错了。进程内那条对照正例必须绿；其余 e2e 用例与 ctest
不受影响——总数按
**974 passed / 9 failed** 读（基线六条＋本步新放的三条；轮 6 实测的基线定义见下面的块）。
> **runner 的 argv 形状（轮 6 实测，全计划五处门统一照这一条写）**：不传 argv 时默认路径指向
> `build/`（`run_e2e_tests.py:26-29`），本机没有这个目录；而 `build-dev/tests/Release/` 那一份
> 虽然存在，**却会让 13 条调试器用例全红**——runner 用
> `dirname(dirname(dirname(nvm)))/ndb/Release/ndb.exe` 拼 ndb（`:117-120`），从
> `build-dev/tests/Release/nvm.exe` 推出来的是 `build-dev/ndb/Release/ndb.exe`，**这个路径不存在**
> （实测 `ls build-dev/ndb` → No such file）。只有 `build-dev/src/tools/nvm/Release/nvm.exe`
> 这一族能推出真实的 `build-dev/src/tools/ndb/Release/ndb.exe`。
> **轮 8 改数**：原稿写「两份 ncc/nvm 的 sha256 相同（`96f2db30…`）」——只有 ncc 那一对是
> `96f2db30…`，nvm 那一对是 `bc096f5b…`（`sha256sum` 四个文件实测；两两各自相同，
> 但 ncc 与 nvm 之间不同，原稿那样写会让人以为四个文件同一个哈希）。
> 结论不变：正例侧没有差别，纯粹是 ndb 定位。
> **基线（轮 6 实测，`d7ca710`，树干净）**：`build-dev/src/tools` 那一条给出
> **974 passed, 6 failed**；`build-dev/tests/Release` 那一条给出 **961 passed, 19 failed**
> （差的 13 条全是 `dbg_*`／`dbgm_*`）。**那 6 条既有失败与本阶段无关**，逐条是
> `cross_mod_default_call_error`（`manifest.txt:596`）、`cross_mod_default_ident_error`（`:597`）、
> `stdlib_io_writefile_arg_compile_error`（`:800`）、`func_ref_tostring`（`:920`）、
> `func_cross_module_reject`（`:962`）、`func_cross_module_arg_reject`（`:964`）——
> **其中第 4 条 `func_ref_tostring` 是「期望编译通过（`manifest.txt:920` 的期望值是 `0`）、
> 实际编译失败」**，其余五条都是「期望 `compile_error`、实际编过」。轮 7 特意把这句话说全：
> 原稿写「前五条怎样、最后一条怎样」，而 `func_ref_tostring` 在六条列表里排第 4，
> 照着原稿的人会先怀疑自己数错，然后开始怀疑六条里到底哪条是既有的。
> 所以本阶段所有 e2e 门的期望数字都写成 **「passed＝974＋此前任务往 `manifest.txt` 新增的行数，
> failed＝6，且失败集合与上面六条逐字相同」**（Task 1 新增三条 `qhead_*` ⇒ 之后各任务门是
> 977/6；实现**之前**跑那三条会红，所以 Task 1 Step 2 的门是 974/9）。出现第七条失败才算本阶段
> 改坏了。**passed 数对不上但失败集合没变＝有新增行没跑到，也算门没过。**
> 这六条与本阶段无关，已报告用户，等他们决定是阶段 5 顺手修还是单独开一张账。
若某条负例今天已经绿了，先停下来在提交说明里写清是哪条、为什么，再决定本任务是否还需要它
——不要为了凑失败去改测试断言。

- [ ] **Step 3: 让展开函数带类型检查**

今天的 `nlang.y:155-172` 是一段注释（`:155-159`）＋`static void CollectQualifiedSegments(...)`
（`:160-172`，无返回值，非 `NK_MemberExpr` 一律 `static_cast<SnMemberExpr*>`）。
**轮 8 改界**：原稿写「把 `:160-172` 整体替换」，而下面的替换体**自带一段新注释**——
只换函数体的话旧的 `:155-159` 五行会留在新注释头上，头一段还在讲旧的空返语义。
所以替换范围是 **`:155-172`**（注释＋函数一起换）。替换体（保持文件内 `\t` 缩进）：

```cpp
//Phase 4b: flatten a MemberExpr point-chain (a.b.c) into identifier
//segments for a qualified type reference. The chain is parsed with the
//normal member-access shifts, then converted only when it is reduced in
//a type position — this keeps a lone identifier a plain NameExpr and
//avoids a reduce/reduce over a single token (a dedicated dotted category
//at a statement head steals '.' from member access; a 2026-09-29 probe
//grammar with such a category measured +2 shift/reduce on '.').
//Returns false when the chain is not made of identifiers (a call, a
//subscript, a parenthesised outer) — the caller marks the node malformed
//and ExprResolver reports it, so no unchecked downcast happens here.
static bool CollectQualifiedSegments(
		SnExpression *pExpr, std::vector<std::string> &out)
{
	if (pExpr->Kind() == NK_IdentifierExpr)
	{
		out.push_back(static_cast<SnIdentifierExpr*>(pExpr)->Name());
		return true;
	}
	if (pExpr->Kind() != NK_MemberExpr)
		return false;
	auto& member = static_cast<SnMemberExpr&>(*pExpr);
	SnExpression* pInner = member.Inner();
	if (pInner == nullptr || pInner->Kind() != NK_IdentifierExpr)
		return false;   //a.b() — the inner side is an InvokeExpr
	if (!CollectQualifiedSegments(member.Outer(), out))
		return false;
	out.push_back(static_cast<SnIdentifierExpr*>(pInner)->Name());
	return true;
}
```

- [ ] **Step 4: `HeadType` 动作改吃返回值**

`nlang.y:1324-1332` 替换为（`SnQualifiedTypeExpr` 今天只有 `first`＋`second` 构造体＋
`AppendSegment`，`SnExpressions.h:575-579`，构造体会无条件 push 两段，所以非法形状必须靠
显式标记，不能靠「少给一段」表达）：

```cpp
			MemberExpr {
				std::vector<std::string> segs;
				SnQualifiedTypeExpr* pQ = nullptr;
				if (CollectQualifiedSegments($1, segs) && segs.size() >= 2)
				{
					pQ = new SnQualifiedTypeExpr(segs[0], segs[1], @1);
					for (size_t k = 2; k < segs.size(); ++k)
						pQ->AppendSegment(segs[k]);
				}
				else
				{
					//Not an identifier chain (a call, a subscript, a
					//parenthesised outer). Keep a typed, unresolved node on
					//the tree so the resolver owns the diagnostic.
					pQ = new SnQualifiedTypeExpr(
						std::string(), std::string(), @1);
					pQ->MarkMalformed();
				}
				$$ = pQ;
				delete $1;
			} |
```

- [ ] **Step 5: 节点补 `MarkMalformed`／`IsMalformed`**

在 `include/nlang/compiler/SnExpressions.h:568-598` 的 `SnQualifiedTypeExpr` 里，与既有
`Segments()`／`AppendSegment()` 并列加（**不加 `SetSegments`**：`AppendSegment` 已经够用，
多一个整体替换入口就是多一条绕过段数不变量的路）：

```cpp
	//Phase 5: a qualified type head whose chain is not made of identifiers
	//(`a.b() v;`) is syntactically reachable but semantically meaningless —
	//the flattener refuses it and the resolver reports it. The node stays
	//unresolved; codegen never sees a resolved malformed chain.
	void MarkMalformed() { m_malformed = true; }
	bool IsMalformed() const { return m_malformed; }
```
并给类补 `bool m_malformed = false;`。同文件 `:562-567` 的「never reaches codegen」注释改成事实：
**已解析的**限定类型节点不可达产码（`VmBackend::Access(SnQualifiedTypeExpr&)` 抛内部错误），
未解析的（含 malformed）在报错后由构建失败拦下。

- [ ] **Step 6: 解析端把内部错误文案换成用户诊断**

`ExprResolverTypes.cpp:332-340` 的既有分支就是这条错误的家（`:337-338` 已经在发
`"Malformed qualified type reference."`），**只加一个条件、不改文案**——本阶段不给同一个
概念第二条诊断：

```cpp
	const auto &segs = qtype.Segments();
	if (qtype.IsMalformed() || segs.size() < 2)
	{
		//A single-segment type uses SnNameExpr; a one-segment qualified
		//node, or a chain that is not identifiers (`a.b() v;`), is refused.
		m_Env.Log(CLL_Error, qtype.Location(),
			"Malformed qualified type reference.");
		return;
	}
```
**轮 8 把这段压成 9 行**（原稿 11 行）。替换锚点 `:332-340` 本来就是 9 行，
而 `ExprResolverTypes.cpp` 在 `d7ca710` 是 498 行、守卫上限 500
（Global Constraints 的余量表同一行写的就是「余量 2」，并且要求**净零行**）——
照原稿写完就正好 500/500 顶格，Task 2／5／6 还要动这个文件，第一个撞墙的人会在
守卫输出里看见自己没碰过的行。所以这里**改两行就得删两行**：把原来的两行注释
（`:335-336`）压成上面这两行，`//` 内容按新事实写（多了 malformed 一支），总行数不变。

- [ ] **Step 7: 账本注释按实测重写＋把冲突数改成机械门**

`nlang.y:1218-1245`（整块，含上面那段 rr 史）替换为：

```cpp
//NameExpr is identifier-only by design. It formerly also derived
//MemberExpr (for `A.B` qualified types) — zero usage in the language,
//and the dual parentage (NameExpr|Expression both deriving MemberExpr)
//was the dominant source of reduce/reduce conflicts. Removing it (plus
//the InterfaceDecl empty-body production) took the grammar from 75 rr
//conflicts down to none, as the re-measure below shows. Removed in the
//Phase 10 audit; do not re-add without a real use.
//Accepted-conflict ledger (re-measured 2026-09-29, bison 3.8.2): 14
//shift/reduce, 0 reduce/reduce, in three families, every one resolved by
//bison's default to the intended reading. The count is pinned by the
//`%expect 14` in the prologue, so bison is SILENT on a clean tree — any
//grammar edit that moves the count now fails the build with
//`error: shift/reduce conflicts: N found, 14 expected` instead of
//leaving a notice in a log nobody reads. Bump `%expect` in the same
//commit as the edit, never in a commit of its own.
//- states 148/185/191 (1 each): the '<' shapes — an explicit generic
//  type head (`List<int> l;`), `new C<T>(...)`, and the void-return
//  Func spellings. Reduce-first keeps the declaration reading.
//- state 254 (9): ClassMember's NodeFlag-singular vs NodeFlags-plural
//  productions overlap on the flag/type first tokens — both derivations
//  parse the same member; shift keeps reading flags.
//- state 290 (2): catch/finally after a nested `try` statement — the
//  dangling-clause shape; shift binds the clause to the innermost try.
```

**范围与两处改动（轮 8 改界＋换机制）**：
1. **替换范围是 `:1218-1245`，不是 `:1229-1245`**（原稿只换下半段）。`:1218-1228` 讲的是
   「NameExpr 曾派生 MemberExpr，删掉之后 rr 从 75 降到 **1**」＋「**剩下的那一条 rr 在 `<` 上**，
   bison 的 reduce-first 默认让它按声明读」——实测今天 **rr＝0**，那条 `<` 上的 rr 已经在
   某个时点自己消失了。只换下半段的话，同一个文件里会留下「上面说有一条 rr、下面说零条」，
   而本步要修的正是这种账实不符。`:1246` 是产生式
   `NameExpr: IdentifierExpr { $$ = new SnNameExpr($1, @1); } ;`，**不在替换范围内**，
   收口时 `sed -n '1246p' src/compiler/grammar/nlang.y` 确认它还在（Files 头一条原来写
   `:1218-1246`，照字面整段替换会把这条产生式一起删掉——轮 8 的实测就是用它当反例）。
2. **在序言加 `%expect 14`**（实测落点：`:8` 的 `%debug` 之后插一行；插在 `:458`／`%%` 之前
   同样通过）。旧注释给的理由——「`%expect` 钉不住这组，只要那条 rr 还在它就会因 rr 报错，
   而 `%expect-rr` 只对 GLR 有效」——**写的时候是对的**，轮 8 另外复测了两半：
   给一条真有 1 rr 的文法加 `%expect 0`，bison 报
   `error: reduce/reduce conflicts: 1 found, 0 expected`、退 1；`%expect-rr 1` 报
   `warning: %expect-rr applies only to GLR parsers`。但 rr 如今是 0，前提消失，
   而它承诺的「build log 里的 notice 当漂移信号」**已经漏掉过一次**：注释记 12 sr＋1 rr，
   实测 14 sr＋0 rr。所以本步把口算的注释换成会失败的机械门。
   **代价要说清再签**：此后任何人改文法，只要冲突数变了就**编不过**，必须在同一次编辑里
   跟着改 `%expect`。本阶段自己就有一次——**Task 2 把 `TypeName` 扩成
   `NameExpr | QualifiedType` 之后实测 16 sr**，那个提交必须同时把 `%expect 14` 改成 16，
   所以 Task 2 的 Files 必须写明序言那行（轮 8 在这里写过一句「轮 8 已挂进 Task 2」，
   **轮 9 实测那是假的**：当时 Task 2 全文 grep 不到 `%expect`；现在真的挂上了，
   见 Task 2 Files 第一条）。
3. **`%expect` 把「门禁」和「测量」拆成两件事，此后全计划四处语法门都要按这两条写**
   （轮 8 提出翻转、轮 9 补上怎么测，因为原稿那三处「期望 N shift/reduce」在 `%expect` 落地后
   全部读不到数字）：
   - **门禁**：`win_bison -d -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y`
     ⇒ **stderr 为空且 rc=0**。bison 在冲突数等于 `%expect` 时**一个字都不打印**；
     `%expect 13` 的对照实测 `error: shift/reduce conflicts: 14 found, 13 expected`、rc=1，
     所以「空输出」在这里是真绿，不是没跑到。
   - **测量**（写账本注释、或 `%expect` 需要跟着改数时）：加 `--report=all` 再读 `.output`，
     实测两条命令（在已带 `%expect 14` 的副本上跑的，正是被静音后的状态）：
     ```bash
     win_bison -d --report=all -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y
     awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}' /tmp/p.output
     awk '/[0-9]+ conflicts: [0-9]+ reduce\/reduce/{n+=$4} END{print "rr="(n+0)}' /tmp/p.output
     ```
     期望 `sr=14`／`rr=0`。**为什么是 `$4`**：`.output` 的行形是
     `State 148 conflicts: 1 shift/reduce`，`$3` 是字面量 `conflicts:`，
     用 `$3` 求和实测得到 `sr=0`——一条永远「零冲突」的假绿测量，比不测更糟。
     逐状态明细用 `grep -n "conflicts" /tmp/p.output`。
   - **先测后 bump 是安全的**（轮 9 实测，这条解掉 Task 2 的顺序死结）：在一条 16 冲突的
     副本上留着 `%expect 14` 跑 `--report=all` ⇒
     `error: shift/reduce conflicts: 16 found, 14 expected` ＋ 一条
     `note: rerun with option '-Wcounterexamples' …`、**rc=1**，
     但 **`.output` 照样写出来**，上面那两条 `awk` 给出 `sr=16`。
     也就是：**要量新数字不需要先把 `%expect` 改对**，rc=1 在这里是预期的不匹配信号，
     量完再 bump；反过来（先 bump 后量）也一样成立。
**轮 5 对实测 ＋ 轮 8 改口径**：上面账本注释里逐条列的状态号 148(1)／185(1)／191(1)／
254(9)／290(2) ＝ **14 sr、0 rr**，与 bison 的总数吻合；分族是**三族**（`<` 形状一族占
三个状态、ClassMember 标志一族、悬垂 catch 一族），原稿这里写「four families」是把「三个状态」
误读成「另外一族」。
**数字的权威是命令本身，不是任何一份现场文件**：`temp/` 永不入库，未来的人（含执行本计划的
agent）拿不到它，要复核就现场跑上面那三条。

- [ ] **Step 8: 跑冲突门＋全量**

```bash
win_bison -d -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y
awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}' /tmp/p.output
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
D:/dev/miniconda3/python.exe tests/e2e/run_e2e_tests.py \
  build-dev/src/tools/ncc/Release/ncc.exe build-dev/src/tools/nvm/Release/nvm.exe \
  | tee /tmp/e2e.log; grep -c '^SKIP' /tmp/e2e.log      # 期望：0
```
（`awk` 那行要先 `win_bison --report=all` 才有 `.output`——本步因为要核对「账本注释里的
14 与实测一致」，把上面第二条命令换成 Step 7 第 3 条那组三条一起跑。）
**期望：`win_bison` 门禁那一行 stderr 完全为空、退出码 0**，测量那一行 `sr=14`。轮 8 翻转：
Step 7 加了 `%expect 14` 之后 bison 在干净文法上**不再打印那行 warning**（实测
`%expect 14` ＝ rc 0 零输出），所以这条门的形状从「读到某行文本」变成「读不到任何文本」。
对照实测：`%expect 13` 报 `error: shift/reduce conflicts: 14 found, 13 expected`、rc=1，
说明门是有牙的，空输出在这里不再是「没跑到」的假绿。
**为什么仍然单独直接跑 `win_bison`，不只看 `cmake --build` 的退出码**（轮 6 定、轮 8 补）：
bison 只在 `src/compiler/CMakeLists.txt:15-21` 的 `add_custom_command … DEPENDS
nlang.y` 触发时才跑，所以不改文法的重跑里构建日志是**空输出**；`%expect` 让「改坏了文法」
编译不过，但让「`%expect` 被人删了」这件事在构建日志里**完全看不出来**。直接跑一次
`win_bison` 与构建解耦，量到的就是文法本身（`/d/dev/win_flex_bison/win_bison` 在 PATH 里，
Global Constraints 已记），产物只落 `/tmp`、不动 `src/compiler/generated/`，工作树保持干净。
ctest **63/63**，
`test_library_source` 里新增**一条函数、一条 `CHECK`**（Step 1 的进程内函数只有对照正例一条
断言；轮 6 纠正：原稿写「三条 `CHECK`」，那三条负例走的是 `manifest.txt`＋runner 子进程门，
不是 ctest 里的 `CHECK`——按字面数断言会以为少了两条）。e2e 门期望 **977 passed / 6 failed**
（基线 974＋本任务放的三条 `qhead_*`；六条既有失败逐字见 Task 1 Step 2 的「runner 的 argv 形状」块，
一条都不能变），SKIP 计数 ＝ **0**。
**轮 5 补**：Step 2 已经跑过 runner，但**这道全量门原本没有它**——本任务往 `tests/e2e/` 放
`qhead_call/`、`qhead_deep/` 两个目录＋`qhead_index.n` 一个单文件并写 `manifest.txt` 的三行，
而 `manifest.txt` 不在 ctest 里（Task 4 Step 11 的轮 4 实测：`grep -n manifest tests/CMakeLists.txt`
＝ 0 命中）。Step 2 之后还要改 Step 3～7 的实现，若终门不重跑 runner，「失败→实现→提交」这条
链的最后一环就没有证据。runner 默认路径指向不存在的 `build/`（`run_e2e_tests.py:26-29`），
必须显式传 `build-dev` 的二进制，而且只能是 `src/tools/…` 那一份（ndb 定位，见上面 Step 2 的块）。

- [ ] **Step 9: Commit**

```bash
git add src/compiler/grammar/nlang.y src/compiler/builder/ExprResolverTypes.cpp \
        include/nlang/compiler/SnExpressions.h tests/test_vm/test_library_source.cpp \
        tests/e2e/qhead_call tests/e2e/qhead_deep tests/e2e/qhead_index.n \
        tests/e2e/manifest.txt
git commit -m "fix(compiler): type-check the qualified type head chain instead of casting it"
```

---

### Task 2: 跨包基类与跨包 `as` 可写

**Files:**
- Modify: `src/compiler/grammar/nlang.y:1056-1057`（`ClassInheritOpt`）、`:1389`（`as`）、
  `:258-262`（`%type`）、账本注释整块（**Task 1 之后它住在 `:1218-1245` 且已被改写成
  「14 sr／0 rr ＋ `%expect 14`」那一版**，本任务在它下面追加，别再照旧稿的 `:1229-1245` 找），
  **以及序言的 `%expect` 行（轮 9 补：Task 1 Step 7 第 2 条承诺「轮 8 已挂进 Task 2」，
  而当时 Task 2 全文没有出现 `%expect` 二字——那是一句指向空气的交叉引用，现在挂上）**：
  本任务把 `TypeName` 扩成 `NameExpr | QualifiedType` 之后实测 **16 sr**，
  所以 `%expect 14` 必须改成 `%expect 16`，**且与产生式改动在同一次编辑、同一个提交里**——
  分两次的话中间那次编不过（`error: shift/reduce conflicts: 16 found, 14 expected`、rc=1）。
  行号以 Task 1 落地后的树为准（`grep -n '%expect' src/compiler/grammar/nlang.y` 应恰好一行）。
- Modify: `src/compiler/builder/StatementResolverTypes.cpp:27-56`（`ResolveClassBases`）
  ——**轮 3 实测：本文件零改动**，`:31` 的 `Resolve()` 是虚分派，限定节点进槽后自动走
  `Access(SnQualifiedTypeExpr&)`。列在这里只是因为它是审核时第一个被怀疑的消费端；
  实施时若发现它确实不用动，就从本提交的 `git add` 里去掉（见 Step 4）。
- Modify: `src/compiler/builder/ExprResolverValues.cpp:115-157`（`Access(SnAsExpr&)`：目标类型名
  在 `:125` 走 `sn.TargetType()->Accept(*m_pVisitor)`，`:130` 取 `->Field()`）
  **不是** `ExprResolverCast.cpp`——那个文件没有 `as` 处理，只有一句 `:327` 的注释提到
  `Access(SnAsExpr)`（轮 3 实测：它的函数是 `LogArrayBindingReject:114`、`FixupParamTypesWithBindings:127`）。
  顺带：`IsResolved()`／`Field()` 都定义在 `SnExpression`／`SnFieldExpr`
  （`SnExpressions.h:141-144`），而 `SnQualifiedTypeExpr : SnCompoundFieldExpr : SnFieldExpr`
  （`:185`、`:568`），所以把 `SnAsExpr` 的目标成员从 `SnNameExpr*` 放宽到 `SnFieldExpr*`
  之后这两处调用不用改。
- Modify: `include/nlang/compiler/SnExpressions.h:660`（轮 7 补进 Files：`SnAsExpr` 的 ctor 形参
  `SnNameExpr*`→`SnFieldExpr*` 就是本任务的**语法面落点**，Step 3/4 与 Step 8 的 `git add` 都要动它，
  而原稿只在上一条的解释性括号里提到这个文件＝「暂存了一个 Files 没声明的文件」。
  实测放宽面**只有 ctor 形参这一处**：访问器 `:675` 与成员 `:694` 早就是
  `SnFieldExpr *`（`:672-674` 的注释写明是 Phase 13 为 alias 预遍剪接而放宽的），
  `:662` 的 `m_pTargetType(pTargetType)`、`:666` 的 `assert`、`:668` 的 `AddChild`
  在形参放宽后照旧成立，一行不用改。）
- Test: `tests/test_vm/test_library_source.cpp`

**Interfaces:**
- Consumes: `SnQualifiedTypeExpr`（已有 `NamespacePath()`／`TypeName()`）、
  `ModuleRegistry::FindModuleType`（`ModuleRegistry.cpp:359-397`）
- Produces: `%type <v_pFieldExpr> TypeName` ＋ 产生式 `TypeName: NameExpr | QualifiedType`；
  `SnAsExpr` **只有 ctor 形参要放宽**：`:660` 是 `SnNameExpr *pTargetType`，而成员
  `m_pTargetType`（`:694`）与访问器 `TargetType()`（`:675`）早在 Phase 13 就已经是
  `SnFieldExpr*` 了（`:672-674` 的注释写明是为别名预处理的克隆节点放宽的），
  所以改动面＝一个形参类型，成员／访问器／`AddChild` 全不动。
  `ResolveClassBases` 一侧不需要新类型：`SuperName()` 今天就是 `SnFieldExpr*`。

- [ ] **Step 1: 写失败的测试**

```cpp
//Phase 5 audit I4: `class D : alib.B` and `x as alib.B` were NameExpr-only,
//so a library type could not be a base class or a cast target.
static void TestQualifiedBaseAndCast() {
    auto dir = scenarioDir("qbase");
    writeFiles(dir, {
        { "alib.n",
          "namespace alib {\n"
          "class B {\n"
          "  public int v;\n"
          "  public int get() { return v + 1; }\n"
          "}\n"
          "}\n" },
        { "main.n",
          "import io;\n"
          "import alib;\n"
          "class D : alib.B {\n"
          "  public int bump() { v = v + 10; return this.get(); }\n"
          "}\n"
          "int main() {\n"
          "  D d = new D();\n"
          "  alib.B b = d as alib.B;\n"
          "  io.print(b.get());\n"
          "  return d.bump() - 11;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "qualified base + cast compile/run");
    CHECK(cap.text == "1\n", "inherited method through qualified base");
}
```
**轮 6 实测订正（这两处不写出来，本条会在 Step 2 白跑一轮）**：把这条 fixture 的等价裸名版
（同文件里 `class D : B`）今天跑一遍，原稿的形状给的是
`Error: The function "get" does not exist or is not accessible.` ＋
`Error: method "get" must be called through a receiver (e.g. this.get(...)).`——
**类的字段和方法跨类可见要写 `public`**（`struct` 不用：实测
`struct Rec { int x; }` ＋ `r.x = 7` 在同一个 TU 外也能读写，见
`tests/e2e/class_basic.n:1` 的 `public` 写法），**方法体内调自己的（含继承来的）方法必须带
`this.`**。加上这两处之后同一形状实测 `Compiled successfully`＋输出 `1`＋`rc=0`
（`io.print` 是「数字＋换行」，进程内 `CapturingIo` 收到的是 `\n` 不是 `\r\n`，与
`test_library_source.cpp:135/409` 的 `cap.text == "ok\n"`／`"7\n"` 同一口径）。
**本计划其余吃 `class` 成员的 fixture 照这一条改**（轮 6 逐个查过，全部已带 `public`）：
Task 4 Step 1 的两份 `Point`、Task 4 Step 1b `(2)` 的 `Box`、Task 4 Step 1b `(5)` 的两份
`Object`。剩下的类型 fixture 都是 `struct`（Task 5 Step 1b 的 `Cfg`、Task 6 `(4a)` 的
`Shade`、`(6)` 的两侧类型），**`struct` 字段不需要 `public`**，别顺手给它们加；
Task 2 Step 7 的 `class D : a.b.C { int z; }` 是负例（期望 `syntax error`），同样不用改。


- [ ] **Step 2: 跑测试确认失败**

期望：编译失败，诊断把 `alib.B` 当未声明名（今天 `':'` 只吃 `NameExpr`，`alib.B` 根本
parse 不出来 → 语法错误）。

- [ ] **Step 3: 加 `TypeName` 范畴**

`nlang.y:1056-1057` 替换，并紧随其后加产生式：

```cpp
ClassInheritOpt:	':' TypeName { $$ = $2; } |
					{ $$ = nullptr; } ;

//A type spelled at a position that has a unique leading token (':' after a
//class name, `as`): the dotted chain may be reduced freely, so NameExpr and
//QualifiedType both derive here without fighting over '.'. Measured (temp/
//probe/d.y): +2 shift/reduce, both on '.', both benign — no production
//expects a '.' AFTER a type name, so bison's shift default is the only
//correct reading. A statement head cannot use this shape (see the HeadType
//comment above).
TypeName:	NameExpr { $$ = $1; } |
			QualifiedType { $$ = $1; } ;
```

`nlang.y:1389` 改为：

```cpp
				Expression KT_As TypeName		{ $$ = new SnAsExpr($1, $3, @2); } ;
```

`%type` 声明区追加 `TypeName`，类型用 `<v_pFieldExpr>`：

```
%type <v_pFieldExpr>				Type TypeArg QualifiedType HeadType TypeName
```

**两处必改的槽类型（轮 2 实测，漏了就编不过）**：
1. `nlang.y:311` 现在是 `%type <v_pNameExpr> ClassInheritOpt` —— 动作 `$$ = $2` 要把
   `TypeName`（`<v_pFieldExpr>`）赋给 `SnNameExpr*` 槽＝非法向下赋值。`:311` 改
   `<v_pFieldExpr>`，**原稿的编辑清单只列了 `:258-262`，必须补 `:311`**。
2. `SnAsExpr` 的**构造函数形参**仍是 `SnNameExpr *pTargetType`
   （`include/nlang/compiler/SnExpressions.h:660`；访问器 `:675` 与成员 `:694` 早在
   Phase 13 就已是 `SnFieldExpr*`）→ `new SnAsExpr($1, $3, @2)` 传 `TypeName` 编不过，形参要一起放宽。

Step 4 的措辞按实测纠正：`SnClassDecl` 的基类槽是 **`SuperName()`，今天就已经是
`SnFieldExpr*`**（`include/nlang/compiler/SnMisc.h:267`，ctor `:261`）；`BaseName()` 返回的是
泛型擦除基类的 `const std::string&`、`SuperClass()` 返回解析后的 `SnClassDecl*`（`:268-269`）
——**原稿点名 `BaseName`/`SuperClass` 是点错了对象**，这两个都不要改，`SnAsExpr` 那边也只有
**ctor 形参**一处要改。

- [ ] **Step 4: 消费端接受两种节点**

只有 `SnAsExpr` 的 target **ctor 形参**需要放宽（上面第 2 条，`:660`；访问器与成员已经是
`SnFieldExpr*`）；`SnClassDecl` 的基类槽
`SuperName()` 今天已是 `SnFieldExpr*`，**不动**，`BaseName()`／`SuperClass()` 也不动
（它们吃的是泛型擦除键和解析后的声明，不是语法槽）。解析处：

- `StatementResolverTypes.cpp:27-56` `ResolveClassBases`：**不用改**（轮 3 实测）。基类槽
  `SuperName()` 今天已是 `SnFieldExpr*`，解析走
  `m_ExprResolver.Resolve(*sn.SuperName(), sn, sn, ERF_None)`（`:31`），而 `Resolve` 的实现是
  `sn.Accept(m_Visitor)` ＋ `return sn.IsResolved();`（`src/compiler/builder/ExprResolver.h:744-751`）
  ——纯虚分派。限定节点一旦进得了这个槽，`Access(SnQualifiedTypeExpr&)`
  （`ExprResolver.h:184` 已声明，`ExprResolverTypes.cpp:327-361` 已实现 import 门＋
  `FindModuleType`＋ owner 过滤）自己就被走到，`:40` 的错误文案用的是 `ToString()`，
  `SnQualifiedTypeExpr` 有 override（`SnExpressions.h:593`）。
  所以本任务的正确做法是**只在语法层放开槽位**，解析层零改动。
- `ExprResolverValues.cpp:115-157` 的 `Access(SnAsExpr&)` 同理：`:125` 已经是
  `sn.TargetType()->Accept(*m_pVisitor)`，也是纯虚分派，唯一要改的是 `TargetType()` 的
  **静态类型**（`SnNameExpr*` → `SnFieldExpr*`，见 Step 3 的 ctor 形参 `SnExpressions.h:660`），
  这样限定节点才塞得进去。`IsResolved()`／`Field()` 在 `SnExpression`／`SnFieldExpr` 上，
  函数体一行不动。

两条都不需要在解析器里加 `Kind()` 分支——加了就是第二套派发规则，比虚分派更容易漏。

- [ ] **Step 5: 账本按实测改总数＋补一族（轮 5：原稿只说「末尾追加」，那样头一句会一直写 14，
  而 Step 6 的门期望 16 ＝ 账本与构建日志互相打脸）**

`nlang.y` 的 Accepted-conflict ledger 头一句里 **`14` → `16`、`three families` → `four families`**，
并在末尾追加第四族：

```cpp
//- states 72/73 (1 each, added with TypeName): ':' / `as` followed by a dotted
//  type name — '.' shifts to continue the chain, the default reduces TypeName.
//  Nothing may follow a type name with '.', so the default is the intended
//  reading.
```

实测依据（**轮 9 换口径**：原来这里引 `temp/probe/d.output:34-40`，探针不入库、引它等于引
一个查不到的出处，与 Task 1 A4 驳回的那条同一类）：当时在探针副本上量到
72(1)／73(1)／145(1)／181(1)／187(1)／250(9)／286(2) ＝ **16 sr**，与 base 相比新增的正是
72／73 两个状态。**执行时以真树的 `--report=all` 输出为准**（命令见 Task 1 Step 7 第 3 条），
逐状态明细 `grep -n "conflicts" /tmp/p.output`，冲突体看
`awk '/^State <N>$/,/^State <N+1>$/' /tmp/p.output`。
**注释里不写规则号**（轮 9）：原稿写 `reduces rule 130 (TypeName)`，`130` 是探针副本的编号，
真树上会随产生式增删平移，钉进提交文本＝钉一个下次改动就失效的数字；按 LHS 名字
（`reduces TypeName`）说就永久成立。
**状态号同理只作为「当时所见」保留**（Task 1 的 148/185/191/254/290 在探针里变成
145/181/187/250/286）：落地时以本次构建的 `.output` 为准逐字改写，别把两轮的编号混进同一份账本。

- [ ] **Step 6: 冲突门＋全量**

```bash
win_bison -d --report=all -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y
awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}' /tmp/p.output
awk '/[0-9]+ conflicts: [0-9]+ reduce\/reduce/{n+=$4} END{print "rr="(n+0)}' /tmp/p.output
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
```
期望：`sr=16`、`rr=0`，**并且第一条命令 rc=0（＝`%expect` 已经跟着 bump 到 16）**。
**轮 9 改这道的形状**：原稿写「16 shift/reduce 读 `win_bison` 的退出流」，而 Task 1 之后
`%expect` 在位、bison **静音**，退出流上什么数都读不到——照原稿执行的人会先看到空输出、
再去找为什么「门没打印」。**测量走 `.output`，门禁走 rc＋空 stderr**（两条分工的实测与
`$4` 陷阱见 Task 1 Step 7 第 3 条）。
如果 `%expect` 还没 bump 就跑到这里：第一条命令 rc=1、报
`error: shift/reduce conflicts: 16 found, 14 expected`，**`.output` 仍然写得出、`sr=16` 仍然可读**
（轮 9 实测），所以先量后改不算违反顺序，但两者必须落在同一个提交里，否则 `cmake --build` 红。
ctest 63/63；`TestQualifiedBaseAndCast` 通过。

- [ ] **Step 7: 负例——三段链（轮 5 按实测改写：原稿的文案与「parse 得下来」两句都不成立）**

实测（`build-dev/tests/Release/ncc.exe build …`，2026-09-30，四份最小源）：

| 拼写 | 今天的真实结果 |
|---|---|
| `class D : a.b.C { … }` | `syntax error`（`ClassInheritOpt` 只吃 `NameExpr`，这正是 Step 3 要放开的槽位） |
| `class D : a.b { … }` | 同样 `syntax error` |
| `a.b.C v;`（声明位） | 解析通过 → `Module 'a.b' is not imported. Add 'import a.b;' … before using type 'C'.` |
| `C y = x as a.b.C;` | 解析通过但**不走限定类型路**：`Cannot resolve the field: a.`（`SnAsExpr` 的目标槽今天只收 `NameExpr`，佐证 Interfaces 段那条 ctor 放宽） |

所以本步的断言**只能在 Step 3～4 落地之后才成立**（语法放开前，`:` 位是 syntax error，钉
resolver 文案＝钉一个还没出生的分支）。做法：Step 3/4 完成后，在 `TestQualifiedBaseAndCast`
同一个文件里加一条

```cpp
//Phase 5 §11: a three-segment type in the ':' position must reach the
//resolver, not the parser. `a.b` is not importable in this phase (dotted
//library imports land in Task 6), so the reachable, stable diagnosis is
//the unimported-module one naming the exact intermediate path.
static void TestThreeSegmentBaseGivesNamedDiagnosis() {
    auto dir = scenarioDir("qbase_three_seg");
    writeFiles(dir, { { "main.n",
        "class D : a.b.C {\n  int z;\n}\n"
        "int main() { D d; return 0; }\n" } });
    const std::string log = compileLog(dir);
    CHECK(log.find("a.b") != std::string::npos,
          "the diagnosis names the exact package path a.b");
    CHECK(log.find("syntax error") == std::string::npos,
          "the ':' slot accepts a dotted type (else Step 3 did not land)");
}
```
**故意不逐字钉整句**：`ExprResolverTypes.cpp:373-377` 那句 `Module '%s' is not imported. Add
'import %s;' at the top of this file before using type '%s'.` 里带 `import` 措辞、Task 6 还会
再动一次文案面；本条钉的是「段名原样出现在诊断里」＋「不再是语法错」这两个本任务的事实。

另一半（「模块存在、类型不存在」⇒ `Type 'C' is not a member of namespace 'a.b'.`，
`ExprResolverTypes.cpp:355`）**在 Task 2 里凑不出前提**：要让 `a.b` 可 import，就得先有带点
库名（Task 6）。所以它排到 **Task 6 Step 1 的 `(4b)`** 旁边（那条已经在钉 `gfx.color` 父段不给
子段的诊断）。**并且**：`:355` 那句文案含 `namespace` 一词，Task 5 Step 6b 会把它改成
`package`——任何任务都**不要**把整句钉进断言，只钉 `is not a member of` 这段稳定前缀。

- [ ] **Step 7b: D9——限定类型上的泛型实参给指名诊断（轮 1 发现整条漏了）**

设计 D9／§4-10 要求：`alib.Vec<int>` 给「用户类型不支持泛型实参」诊断，`alib.Vec` 正常使用。
**今天的真实行为是裸语法错误**：泛型拼写只挂在 `IdentifierExpr` 上
（`nlang.y:1311/1314/1319` 三条 `IdentifierExpr '<' ...`），`MemberExpr`／`QualifiedType`
都没有 `'<'` 分支。
**轮 3 实测（2026-09-29，`build-dev/tests/Release/ncc.exe`，`alib.Vec<int> v;`）**：

```
main.n(line 3, char 18): Error: syntax error
main.n(line 3, char 3): Error: Invalid statement.
Compilation failed.
```
exit 1，**没有**内部错误／崩溃——`<` 直接被判语法错，指针停在链头之后。
所以走**第一条路**：只加测试钉住这个形状（断言 `rc != 0` 且日志含 `syntax error`，
断言日志里**不含** `Compiler internal error`），**不加产生式、不改账本**，14/16 的期望值不变。
设计 D9 要的「用户类型不支持泛型实参」指名文案**不在本阶段做**：它需要一个能吃下
`QualifiedType '<' ... '>'` 再报错的恢复产生式，那是新增 LALR 状态、要重测冲突面的活，
收益只是一句更客气的文案。把它记进阶段 7 的缺口清单（§「未排期」），本阶段用实测文案钉住。

内建泛型的擦除键不受影响：`GetGenericClassDecl`（`ExprResolverTypes.cpp:103-152`）只对
内建／已实例化的头名字生效，`List<int>`／`Dict<k,v>` 仍走 `IdentifierExpr` 分支。
同时补 §4-10 的正例：`alib/vec.n` 里声明 `Vec`、调用点写 `alib.Vec`（不带实参）必须通过。

**两条都要落成代码，别只留描述（轮 6 自查：这一段原本只有「补一条测试」这句话，
是占位符）**。落点 `tests/test_vm/test_library_source.cpp`，helper 全用现成的
`scenarioDir`／`writeFiles`／`compileDir`／`compileRun`／`compileLog`（Task 1 加的），
函数名与 `CHECK` 风格同文件：

```cpp
//(§4-10 负例) D9：限定类型上写泛型实参今天得到裸语法错误。本阶段只钉「不误接受」，
//指名文案是阶段 7 的缺口——所以断言吃的是实测到的那句，不是愿望。
static void TestQualifiedGenericArgStaysRejected() {
    auto dir = scenarioDir("qgen_neg");
    writeFiles(dir, {
        { "alib.n", "namespace alib {\nstruct Vec { int x; }\n}\n" },
        { "main.n", "import alib;\nint main() {\n"
          "  alib.Vec<int> v;\n"
          "  return 0;\n}\n" } });
    CHECK(!compileDir(dir), "a generic argument on a qualified type is rejected");
    const std::string log = compileLog(dir);
    CHECK(log.find("syntax error") != std::string::npos,
          "the measured rejection is the grammar one (D9 deferred wording)");
    CHECK(log.find("Compiler internal error") == std::string::npos,
          "and it is a diagnostic, not an internal failure");
}

//(§4-10 正例) 同一份库源，不带实参的限定拼写要能声明、能读写字段；
//内建泛型的擦除键（"List"/"Dict"）在同一条程序里一起用，钉住「限定面没有动到内建」。
static void TestQualifiedLibraryTypeWithoutArgsWorks() {
    auto dir = scenarioDir("qgen_pos");
    writeFiles(dir, {
        { "alib.n", "namespace alib {\nstruct Vec { int x; }\n}\n" },
        { "main.n", "import alib;\nint main() {\n"
          "  alib.Vec v; v.x = 2;\n"
          "  List<int> nums;\n"
          "  return v.x - 2;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0,
          "alib.Vec (no args) compiles and runs beside a builtin generic");
}
```
**注册**：两条都要加进该文件 `main()` 里的调用串（与 Step 1 的
`TestQualifiedBaseAndCast` 同一串，手写注册、不自动发现）。
`List<int> nums;` 这一句轮 6 实测：与一个同 TU 的 `struct Vec` 并存时今天就是绿的
（`ncc` ＋ `nvm` 双绿，`rc=0`）——把它并进正例是为了让「限定面误伤擦除键」在同一场景里
当场红；**只声明不调方法**，别再引一个成员名进去给本步添变量。

这一格在原稿里是空的（审阅轮 B 就是从这找出来的）；**不要用 Step 7 的三段链
（`class D : a.b.C`）顶替**——那是 §4-11 的格子，不是 §4-10。

- [ ] **Step 7c: 重跑冲突门＋全量（Step 6 之后改了语法，必须再量一次）**

Step 6 的 16/0 是在 Step 7b 之前测的。若 7b 走了「只加测试」那条路，数字不变，这一步只是
复跑确认；若 7b 加了 `error` 恢复产生式，**Step 5 的账本追加文字与 Step 6 的期望值都要就地
改成实测数字**（不许留「16＝14＋2」的旧账），然后：

```bash
win_bison -d --report=all -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y
awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}' /tmp/p.output
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
```
期望：`awk` 的 `sr=` 与账本注释里写的数**逐字相符**，**且第一条 rc=0（`%expect` 与它同步）
**；ctest 63/63；Step 7／7b 的新断言全绿。（轮 9：原稿写「`win_bison` 报的 sr／rr 数」，
`%expect` 在位后它什么都不报——测量只走 `--report=all` 的 `.output`，见 Task 1 Step 7 第 3 条。）

- [ ] **Step 8: Commit**

```bash
git add src/compiler/grammar/nlang.y \
        src/compiler/builder/ExprResolverValues.cpp include/nlang/compiler/SnExpressions.h \
        tests/test_vm/test_library_source.cpp
git commit -m "feat(compiler): qualified type names as base class and cast targets"
```
`StatementResolverTypes.cpp` **不进这个提交**（轮 3 实测：`Resolve()` 是虚分派，它不需要改）。
`ExprResolverValues.cpp` 只有当 `Access(SnAsExpr&)` 函数体真的动了才进；若唯一的改动就是
`SnExpressions.h:660` 的 ctor 形参放宽，那也从 `git add` 去掉——提交说明里写清实际改了哪几处。

---

### Task 3: 路径派生的包名缝（owner 基）——**纯加法，不动任何判定**

> **轮 1 审核改写了本任务的边界。** 原稿在这里同时做「加 `PackageOf`」和「退役两处容器判定」，
> 后者在 `namespace` 壳还在的树上必然丢失全部库符号（见前置事实），Task 3 的门就红了。
> 现在本任务**只加两条没人调用的函数**，判定退役搬到 Task 5 Step 3b，与去壳同一次提交。

**Files:**
- Modify: `src/compiler/builder/ModuleRegistry.h`（**轮 5 订正指针**：新公开两函数插在 `OwnerOf`
  的声明 `:173` 之后、`:175` 的 `OwnerOfContext` 注释之前——原稿写「`:160-164`」，那一段是
  `TagOwner`／`EraseOwner` 的注释体；也别再声称「`:203-208` 是 `OwnerOf` 等内联访问器」，
  `:203-208` 实测是**私有**的 `CompiledInFunctions` 声明，`OwnerOf` 在 `:173`）
- Modify: `src/compiler/builder/ModuleRegistry.cpp`（两个定义追加在 `OwnerOfContext` 定义
  （`:415` 起）之后、`:433` 的 `} //namespace nlang` 之前。**轮 5 纠正**：原稿说「`:357` 的
  `namespace` 闭合之前」——`:357` 关的是 `:340` 打开的**匿名**命名空间（`namespace nlang` 直到
  `:433` 才关），把 `ModuleRegistry::PackageOf` 这类外层类的成员定义放进匿名 namespace 是
  ill-formed，编译器直接报错，不是「放错位置」而是**构建失败**。放在 `:340` 之前同样错。）
- Test: `tests/test_compiler/test_module_import.cpp`

**Interfaces:**
- Consumes: `ModuleRegistry::OwnerOf(const SnField&)`（已有）、`ModulePathOf(uint32_t)`（已有）、
  `m_modules[i].path`（`DeriveModulePath` 产出的点分路径）。
- Produces：
  ```cpp
  //ModuleRegistry.h, public section
  std::string PackageOf(const SnField& member) const;   //"" when unowned
  std::string QualifiedName(const SnField& member) const;
  ```
  Task 4 的产码侧与 Task 5 的字面量改写都用这两个名字。

- [ ] **Step 1: 写测试——注册表层的名字派生**

本提交没人调用这两个函数，所以能绿的门只有「函数本身算对」。测在
`tests/test_compiler/test_module_import.cpp`。**这个文件的形状轮 3 实测纠正**：它是
**QtTest** 用例类（`class TestModuleImport : public QObject` `:401`，`private slots:` `:404`，
`QTEST_GUILESS_MAIN` `:1585` 左右），**槽函数由 moc 自动发现，不需要在 `main()` 里注册**
（原稿那句「必须注册进 main 调用串」是从 `test_library_source.cpp` 那套手写 `CHECK` 文件
串了门——那个文件才需要注册，两处别再混）。脚手架直接用现成的
`buildGateProject(GateProjectOptions)`（`:203`）＋ `moduleIndexOfPath`（`:136`）＋
`findMember`（`:149`）＋ `res.builder->TreeRootView()`：

```cpp
    //Phase 5 R1/D8: the package name is the OWNING unit's path, not the AST
    //namespace chain. Untagged members (built-ins, synthetic generic
    //instantiations) have no package and stay bare.
    void packageOfIsOwnerDerived()
    {
        GateProjectOptions opts;
        opts.szMainBody = "int main() { return 0; }\n";
        opts.szHelperBody = "int help() { return 3; }\n";   //proj/utils/helper.n
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "gate project with a cross-directory module must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const SnNamespace& rootView = res.builder->TreeRootView();

        //(a) project member in a nested directory: path "utils.helper",
        //    so the key is utils.helper.help -- the path, not any wrapper.
        QVERIFY(moduleIndexOfPath(reg, "utils.helper") != ModuleRegistry::NO_OWNER);
        const SnField* pHelp = findMember(rootView.Members(), NK_Function, "help");
        QVERIFY2(pHelp != nullptr, "help() must reach the merged root");
        QCOMPARE(reg.PackageOf(*pHelp), std::string("utils.helper"));
        QCOMPARE(reg.QualifiedName(*pHelp), std::string("utils.helper.help"));

        //(b) the root project unit is package "main" (stem of main.n),
        //    so its entry key is main.main -- D13's visible spelling.
        const SnField* pMain = findMember(rootView.Members(), NK_Function, "main");
        QVERIFY(pMain != nullptr);
        QCOMPARE(reg.QualifiedName(*pMain), std::string("main.main"));

        //(c) D12 anchor: a stdlib member keeps today's key. `io.print` is
        //    reached through the namespace container the shell still
        //    provides on this tree, and its owner tag is the unit whose
        //    path is "io" -- same string, different source of truth.
        //    Measured fix (round 4): the library unit only reaches the AST
        //    root when it is import-reachable (ModuleBuilder::PrepareUnits ->
        //    DiscoverLibraryUnits, ModuleBuilder.cpp:71), so this case needs
        //    its OWN gate project with `import io;` -- the shared `opts`
        //    above has no import and would leave `io` out of the root,
        //    making the QVERIFY2 below red for a reason unrelated to R1.
        GateProjectOptions ioOpts;
        ioOpts.szMainBody = "import io;\n"
                            "int main() { io.print(1); return 0; }\n";
        auto ioRes = buildGateProject(ioOpts);
        QVERIFY2(ioRes.ok, "gate project importing stdlib io must build");
        const ModuleRegistry& ioReg = ioRes.builder->Registry();
        const SnNamespace& ioRoot = ioRes.builder->TreeRootView();
        const SnField* pIo = findMember(ioRoot.Members(), NK_Namespace, "io");
        QVERIFY2(pIo != nullptr, "stdlib io unit must be merged into root");
        const SnField* pPrint = findMember(
            static_cast<const SnNamespace*>(pIo)->Members(), NK_Function, "print");
        QVERIFY2(pPrint != nullptr, "io.print must be indexed");
        QCOMPARE(ioReg.PackageOf(*pPrint), std::string("io"));
        QCOMPARE(ioReg.QualifiedName(*pPrint), std::string("io.print"));


        //(d) unowned member degrades to the bare name. The merged root
        //    container itself carries no owner tag, so this is the free
        //    NO_OWNER case (a synthetic generic instantiation would do
        //    too, but needs a compile to mint).
        QCOMPARE(reg.PackageOf(rootView), std::string());
        QCOMPARE(reg.QualifiedName(rootView), rootView.Name());
    }
```
`(d)` 若实测发现 `OwnerOf(rootView)` 不是 `NO_OWNER`（root 被打了标签），**换成**
`GetGenericClassDecl` 为 `List<int>` 铸出的合成 `SnClassDecl`（它不进注册表），
不要为了凑绿把断言删掉——(d) 钉的就是「未打标签 ⇒ 裸名」这条回落，
D5 的内建 `List`/`Dict`/`Object` 全靠它。

（本文件里所有 `GateProjectOptions` 变体的可用字段见 `:192-201`；`io` 那条依赖
`params.m_sStdLibDir = STDLIB_DIR`（`:105`，编译定义），stdlib 的 `print` 若在
`io.n` 里改名，(c) 的断言跟着改，别改成宽松匹配。）

- [ ] **Step 2: 跑它确认失败**（未实现 → 链接失败或断言失败）。

行为面的正例留到能让它们绿的提交再写，**去向轮 4 重新对过号（原稿两条都指错）**：

- 设计 §2（`a/io.n` 与 `b/io.n` 同末段共存）：要**带点 `import`** 才凑得出两条独立包路径，
  落在 **Task 6 Step 1**，不是原稿写的「Task 5 Step 4」（那一步是 bison 冲突计数，没有
  编译＋执行的形状）。
- 设计 §3（项目侧 `utils/helper.n` 与 `core/helper.n` 各一个 `help()` ＋同名 `Cfg`）：
  原稿**没给任何任务**，现落在 **Task 5 Step 1b**（去壳是它最大的回归风险，钉子放在
  那次提交的门里才有意义）。
- 设计 D8（路径压过壳名）：原稿说留给「Task 6 Step 1」，实际**已经写掉了**——
  Task 4 Step 1b 的 `(1)` `TestPathBeatsShellName` 就是它，别再重复排期。

**本任务不放编译＋执行级测试**，理由记进提交说明，别让后面的审阅者以为漏了。

- [ ] **Step 3: 实现两条缝**

`ModuleRegistry.cpp` 末尾（owner 访问器旁边）加：

```cpp
std::string ModuleRegistry::PackageOf(const SnField& member) const
{
	//The unit's dotted path is the package name; an untagged member
	//(built-in type, synthetic generic instantiation) has no package.
	//Deliberately NOT derived from the AST namespace chain: the container
	//exists only where a library file literally writes `namespace <path>`,
	//so the chain cannot express a path-derived package for project units.
	const uint32_t owner = OwnerOf(member);
	if (owner == NO_OWNER || owner >= m_modules.size())
		return std::string();
	return m_modules[owner].path;
}

std::string ModuleRegistry::QualifiedName(const SnField& member) const
{
	const std::string pkg = PackageOf(member);
	if (pkg.empty())
		return member.Name();
	return pkg + "." + member.Name();
}
```

声明进 `ModuleRegistry.h` 的公开段，注释写明调用契约：**只对合并进 root 的顶层成员
／库成员有效**；类方法的裸名规则由调用方保留（Task 4）。

- [ ] **Step 4: 本任务**不**退役容器判定（轮 1 纠正）**

`CompiledInFunctions`（`ModuleRegistry.cpp:280-311`）与 `FindModuleType`（`:359-397`）的
`isLibrary ? pRoot->FindField(path) : root` **原样留着**。原因见前置事实：库成员此刻还住在
`namespace <path>` 壳里，root 上的 `equal_range` 扫不到它们，现在就删会当场丢光 `io.print`
一类的解析。删除动作与代码放 **Task 5 Step 3b**（那里去壳，成员才真正落到 root）。

- [ ] **Step 5: 全量**

`cmake --build build-dev --config Release -j 8 && ctest --test-dir build-dev/tests -C Release`
期望 **63/63，且本任务不可能红**：只新增了无人调用的两个函数与一个新注册的用例
（新用例在既有 `test_module_import` 可执行文件里，不新增 ctest 条目，所以总数不变）。
若这里就红了，说明顺手改了判定逻辑——回退到只有加法。

- [ ] **Step 6: Commit**

```bash
git add src/compiler/builder/ModuleRegistry.h src/compiler/builder/ModuleRegistry.cpp \
        tests/test_compiler/test_module_import.cpp
git commit -m "refactor(compiler): derive a unit's package name from its owner tag"
```

---

### Task 4: VM 侧限定键——函数、类型、磁盘、入口点（一次闭合）

**必须一次提交、一次转绿**：键的写入端与所有读取端分开改会全线红。
本任务改完后，stdlib 的键**一字不变**（D12），项目侧全部变长。

**Files:**
- Modify: `src/compiler/builder/ModuleRegistry.h`（`QualifiedName` 供 VM 调用，已在 Task 3）
- Modify: `src/vm/CMakeLists.txt:55-61`（`target_include_directories(nlang_vm ...)`，PRIVATE 段
  只有 `:60` 一行 `${CMAKE_CURRENT_SOURCE_DIR}`；在 `:59` 的 `PRIVATE` 下追加
  `${PROJECT_SOURCE_DIR}/src/compiler`。**轮 3 勘误**：`:48-50` 是源文件列表
  （`Disassembler.cpp`/`DisassemblerWalk.cpp`/`ModuleLoader.cpp`），不是 include 块，
  文件里今天**没有**任何指向 `src/compiler` 的 include 目录。链接方向已经有了：`:65` 的
  `PRIVATE nlang_compiler`）
- Modify: `src/vm/VmBackend.h:80-90`（`SetModuleRegistry`）＋ `:149-`（成员 `const ModuleRegistry* m_pRegistry = nullptr;`）
  ＋ 日志通道注入（`VmBackend.h` 里**没有** `m_Env` 成员，只有 `SaveModule(BuildEnvironment&)`
  形参 → Step 6 的查重诊断必须走显式注入，见该步）
- Modify: `src/compiler/ModuleBuilder.cpp:185-192`（注入，紧邻 `SetImportedModules`／`SetLibraryIndex`）
- Modify: `src/vm/backend/Register.cpp:18-41`（缝）＋ `:47`（`cs.name` 写入）＋
  `:62-66`（`m_structFieldTypeNames` 的写入：`typeNames.push_back(fieldType->Name())` 在 `:63`）＋
  `:86`（`FindStruct`）＋ `:101`（`FindClass`）＋ `:122/126`（元素型查表）＋ `:272`
  （**轮 3 勘误：原稿列的 `:108-113` 不是入表点**，那是 `ResolveStructClassRefs` 里的
  v1.12 类型描述符注释＋`BuildTypeDesc` 循环；`RegisterStructs` 在 `:73`、
  `RegisterStructDecl` 在 `:45-71`）
- Modify: `src/vm/backend/VmBackend.cpp:66-69`（`GenerateAllBytecode` ＝ entryPoint 的写入收口）、
  `:128-136` ＋ `:182-190`（`FillNativeFunctionRecord` 是 `isNative` 的唯一编译器写入点，D13 的
  native 入口诊断放这儿）、`:454`（`SaveModule`，只读 `entryPoint`）
- Modify: `src/vm/backend/RegisterClass.cpp:95/100/113/117/157-158/161/176/201`、
  `src/vm/backend/Register.cpp:86/101/122/126`
- Modify: `src/vm/backend/EmitCall.cpp:114`、`EmitExprCast.cpp:279`、`EmitExprInitList.cpp:131/345`、
  `EmitStmtAssign.cpp:62/311`、`EmitStmtDecl.cpp:35`、`EmitStmtForeach.cpp:251`、
  `EmitStmtStore.cpp:149`（轮 1 补：这些 AST 名查表原来不在表内，静默返回 −1）
- Modify: `src/vm/backend/Import.cpp:236-263`
- Modify: `include/nlang/vm/CompiledModule.h:43`（12→13）、`:295-306`（`std::string name;`
  之后新增 `entryPoint`）
- **不删任何 finder**（轮 2 实测，原稿的「删除 `FindFunction`/`FindStruct`/`FindClass`/
  `FindEnum` 四个按名查找入口」三处都不成立）：`FindEnum` 在本仓库**不存在**；
  `FindStruct`（`include/nlang/vm/CompiledModule.h:314`）有 **15** 处、`FindClass`（`:321`）有
  **19** 处调用点（轮 3 重数：命令是 `grep -rn "FindStruct(" src include \| grep -v "int FindStruct"`，
  原稿的 12／18 少算了），它们就是下面 Step 5 表的读取端，删了整张表无处可落；`FindFunction`（`:307`）
  在 Step 8 之后只剩测试调用点（`tests/test_vm/test_debugger.cpp` 的 `FindFunction("main")`
  **6 处**：`:858/:892/:929/:973/:1273/:1766`，
  `tests/test_vm/test_vm.cpp:181-183` 3 处），删它等于给测试手写扫表——本阶段保留，
  键限定后测试只是改拼写（`:181-183` 的 `foo`/`bar`/`baz` 是项目裸名，不变）。
- Modify: `src/vm/ModuleSaver.cpp:39-42`（版本号写入，12→13 的那一位在 `:40` 的
  `NMOD_FORMAT_MINOR`，轮 6 实测订正：原稿写的 `:78` 是 `nativeFlag`，与格式版本无关）、
  `src/vm/ModuleSaver.cpp:44-47`（模块名写出，`entryPoint` 紧跟在这段之后）、
  `src/vm/ModuleLoader.cpp:67`（地板）
  ＋ `:77-83`（读回 `name` 处补 `entryPoint`）＋ `:219/288/347/376`（四个恒真的
  `if (minorVer >= 12)` 门，见 Step 8 第 2 条）。`:124-136` 是 `intrinsicId`／`nativeFlag`
  的读取，**与本任务无关，别改**（原稿把它说成「导入模块 stub」路径，那个东西不存在）。
- Modify: `src/vm/VmExecutor.cpp:89-91`（入口改读 `entryPoint`）；`:24-47` 的
  `List`/`Dict`/`Exception` 等**内建字面量缓存保持不变**（内建无 owner ⇒ 键就是裸名）
- Modify（Object 派生判定，轮 2 实测重新定位）：`src/vm/backend/RegisterClass.cpp:173-179`
  （`ApplyImplicitObjectInheritance` 的 `cc.name != "Object"`）、
  `src/vm/VmExecutorOpsObjects.cpp:321-330`（`ReceiverClassIndex` 的 `classes[i].name == "Object"`）、
  `src/compiler/builder/BuiltinNames.h:28-31`（`IsBuiltinClassName` 白名单，编译器侧，裸名）
  ——三处都**按裸名比**，限定后只有无 owner 的内建 `Object` 命中，语义正好；本步只加注释钉住
  「裸名＝内建」，不引入第二种判定。（原稿写的 `include/nlang/vm/VmExecutor.h:398-407`
  ＋`InitStructHeap`＋`FindEnum` 经轮 2 实测**全部不存在**：`VmExecutor.h` 在 `src/vm/`，
  `:398-407` 是 NativeHost 回调；`InitStructHeap` 在 `src/vm/VmExecutor.cpp:69-80`，只清堆栈、
  不查名；本仓库没有 `FindEnum`。`GetOrMatchStruct`／`g_lastCaller` 同样不存在，别去找。）
- Modify: `src/vm/VmExecutorDebug.cpp:76/84`（`info.funcName = frame.func->name`，D13）、
  `src/vm/DebugSessionController.cpp:34`（`LocationLabel(func.name,…)`）＋ `:139-165`
  （`AddFunctionBreakpoint` 全体；裸名比较在 `:152`，循环体 `:150-158`。轮 4 实测：本仓库
  **没有 `AddLineBreakpoint` 这个符号**（`grep -rn AddLineBreakpoint src include tests` ＝ 0 命中），
  原稿那句「`:155` 起是 `AddLineBreakpoint`」作废；`:155` 是同函数里的 `if (bp.anchors.empty())`）
- Modify: `src/vm/VmExecutorOpsCalls.cpp:300-315`（**没有 `backend/` 这一层**，轮 4 实测：
  `FindMethodByName` 定义在 `:300`、裸名比较在 `:309`＝D8 的依据，保持裸名并加注释）。
  （原稿的 `VmExecutorFormat.cpp:238/247-251` 与
  `VmExecutorOpsCalls.cpp:345-360` 经实测**没有按名查表**，条目作废。）
- Modify: `src/vm/backend/EmitExprNew.cpp:55-56`、`EmitExprInitList.cpp:178/232/305-307`、
  `EmitStmtSwitchTry.cpp:312-316`＋`:428`、`src/vm/TypeDesc.cpp:179/229`（＋`:203/214` 的
  `BaseName()=="List"/"Dict"` 内建比较，保持裸名）、`src/vm/VmExecutorOpsObjects.cpp:326`、
  `src/vm/VmExecutor.cpp:26/32/38`
- Modify: `src/compiler/builder/CompiledModuleNodeBuilder.hpp:425`（`cf.name == "main"` 的
  入口节点判定；`:107-112` 是同名重复字段拒绝、`:225/230` 是 native 名／DLL，与入口无关）
- Modify: `src/vm/VmExecutorSer.h:204-206/290-304`、`src/vm/IntrinsicsByteStream.cpp:208/256`、
  `src/vm/IntrinsicsFileStream.cpp:220/270`（写读同源，代码不改，各加一行注释说明它跟随表键）
- Modify: `tests/packaging/verify_package.py:51`、`src/vm/TestNatives.h:61-64`（**唯一的注册串
  字面量所在地**，注册名改限定名）。三个工具 main **不逐名改**：它们只调
  `RegisterTestNatives(executor)`（轮 4 实测行号：`src/tools/ncc/main.cpp:131/381`、
  `src/tools/ndb/main.cpp:56/159`、`src/tools/nvm/main.cpp:99`；原稿列的 ncc `:35` 是 usage
  文本、ndb `:42` 是 `BuildLibrarySearchPath` 循环、nvm `:36` 是 `--version` 分支，都与注册无关），
  所以 main 里没有任何 native 名字面量要动——除非 Step 10 的 D5 诊断要新增文案，那才进 main 面。
  同批：`src/tools/ndb/MachineFrontEnd.cpp:118-133`（`b <file> <line>`，
  `:126` 的 `b expects <file> <line>`、`:128` 的 `atoi`）＋ `:135-158`（`bfunc <name>` 的按名
  扫表，`:144-145` `func.name != name`）、`src/tools/ndisasm/main.cpp:28/93/114/146`
  （打印函数名／struct／class 表，`:146` 的 `-func <name>` 过滤是**用户可见拼写**，D13 面；
  原稿的 `src/tools/ndisasm/Disassembler.cpp:36-40` 不存在，那个目录只有 `main.cpp`＋`CMakeLists.txt`）
- Modify: `src/compiler/builder/ExprResolverMemberBuiltins.cpp:286-318`（D10 字面量改写）
- Test: `tests/test_vm/test_library_source.cpp`、`tests/test_vm/test_debugger.cpp`（含
  **三处** `minorVer` 版本钉子 `:254`／`:300`／`:348`（断言文案在 `:256`／`:302`／`:350`，
  降级字节分别是 `:257`=0x09、`:303`=0x0A、`:351`=0x0B）——轮 4 实测：原稿只列了两处，
  第三处漏了；`0x0C`→`0x0D` 三处一起改，见 Step 8）、
  `tests/test_nide/test_debug_client.cpp`、`tests/test_nide/test_mainwindow.cpp`、
  `tests/e2e/manifest.txt`（14 条含 `native` 的用例：`native_*` 11 条 ＋ `func_*native*` 3 条）
  ＋ 金样本 `manifest.txt:1021/1022/1025/1026/1029`（D13，全表见 Step 11）
- Modify: `tests/CMakeLists.txt`（**新增一条 `add_test` ＝ 本计划全篇唯一的 ctest 条目新增**，
  代码与理由见 Step 8c；门限由 63/63 变 64/64，Global Constraints 已按任务列出门限曲线）

**Interfaces:**
- Consumes: Task 3 的 `PackageOf`／`QualifiedName`；`ModuleRegistry::NO_OWNER`。
- Produces: `CompiledModule::entryPoint`（`int32_t`，-1＝无）；
  `VmBackend::SetModuleRegistry(const ModuleRegistry*, BuildEnvironment*)`（一个注入点带两样：
  拼写来源＋诊断通道，不设第二个 setter）；
  **`std::string VmBackend::KeyOf(const SnField&) const`**——**声明在 `VmBackend.h`**（类成员，
  Step 5 的表要从 `backend/` 下约十个 `Emit*.cpp`／`RegisterClass.cpp` 调它，不声明进头文件就
  链接不到），**定义在 `Register.cpp`**；
  它内部用的二参自由函数 `QualifiedName(const ModuleRegistry&, const SnField&)` 才是
  `Register.cpp` 的文件级内部符号，**不进任何头**。
  （**轮 5 消除自相矛盾**：原稿 Interfaces 写「`KeyOf`……`Register.cpp` 内部，不导出」，
  Step 4 又写「把 `QualifiedName` 声明进 `VmBackend.h`」，而 Step 5 的表满仓调 `KeyOf`——
  三句话互相做不到。导出的就是 `KeyOf`；`ModuleRegistry::QualifiedName(field)` 是 Task 3 的
  resolver 侧缝，方法裸名那条规则只有产码侧需要，所以外部要限定名的调用方仍走 Task 3 的缝。）

- [ ] **Step 1: 写失败的测试——同名类型不再串布局（rb3 升级）**

```cpp
//Phase 4b-2 audit I3 (reproduced in temp/rb3): with bare-name keys, a
//library Point and a root Point collide and the later one silently gets the
//first one's field layout. Qualified keys must isolate them by value.
static void TestSameNameLibraryAndRootTypeIsolated() {
    auto dir = scenarioDir("rb3_named");
    writeFiles(dir, {
        { "alib.n",
          "namespace alib {\n"
          "class Point { public int x; public int y;\n"
          "  public int sum() { return x + y; } }\n"
          "}\n" },
        { "main.n",
          "import io;\n"
          "import alib;\n"
          "class Point { public int a; public int b; public int c;\n"
          "  public int id() { return a + b + c; } }\n"
          "int main() {\n"
          "  alib.Point p = new alib.Point();\n"
          "  p.x = 1; p.y = 2;\n"
          "  Point q = new Point();\n"
          "  q.a = 4; q.b = 5; q.c = 6;\n"
          "  io.print(p.sum());\n"
          "  io.print(q.id());\n"
          "  return p.sum() + q.id() - 18;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "isolated layouts run clean");
    CHECK(cap.text == "3\n15\n", "library sum(1+2)=3, root id(4+5+6)=15");
}
```
**轮 6 实测订正（原稿这十五行有两类硬伤，跟着做会把阶段 5 的旗舰用例做成空转）**：

1. **数值自相矛盾**：原稿写的是「库 `sum()` 返回 `x + y`、根 `id()` 返回常量 `100`，
   然后断言 `cap.text == "3\n"` 且退出码 `0`」。实测那个形状的输出是 `100`、
   退出码是 `100 + 0 - 3 = 97`——**两条断言在功能完全正确的实现上也是红的**。
   唯一的收尾办法是把断言弱化成 `rc == 0`／不比输出，而那正好把设计 §1 要的
   「断言数值」删干净了，Step 13 的负控也骑在同一条断言上。现在这版每条数字都是
   从 fixture 算出来的：`p.sum()=1+2=3`、`q.id()=4+5+6=15`、输出两行 `"3\n15\n"`、
   退出码 `3+15-18=0`。**布局一串就两处都错**（`p` 拿到三字段的偏移、`q` 拿到二字段
   ⇒ 读值错位或 `struct field store out of bounds`），这才叫数值判别。
2. **`public` 缺失**：原稿两份 `class` 的成员都不带 `public`，跨类调用直接
   `Error: The function "id" does not exist or is not accessible.`（同 Task 2 Step 1
   那个块里实测的第二条硬伤）。方法体内自调用还得写 `this.`，本条没有自调用，不需要。

- [ ] **Step 1b: 同一文件里再补五条用例（轮 2/3/4 实测：原稿的测试面缺这几条，
> 而它们正是 Step 6／Step 8／Step 9／Step 10 那四处改动的门）**

**轮 4 实测订正：这个文件里只有两个编译入口，原稿代码块写的
`compileDirOnly(dir2, &pLib)` 和三参 `compileRun(dir, cap, &pMod)` 都不存在**：

- `bool compileDir(const fs::path& dir)`（`tests/test_vm/test_library_source.cpp:68-84`）
  ＝只编译、返回 `Build()` 的成功位；入口源文件名硬编码 `main.n`（`:70`）。
- `int compileRun(const fs::path&, CapturingIo&)`（`:90-100`）＝编译＋执行，返回程序退出码。

读回 `CompiledModule` **不需要新入口**：`compileRun` 自己就是这么拿模块的——
`:94-95` `CompiledModule mod = ModuleLoader::Load((dir / (modName + ".nmod")).string());`，
**按值返回**（不是指针，别写 `->`）。所以下面凡是要看模块内容的地方，一律
`compileDir(dir)` ＋同一行照抄那句 `ModuleLoader::Load(...)`；不给 `compileRun` 加出参，
也不造第三个 build＋load 入口。库单元那份模块没有 `main.n`，所以本步骤给 `compileDir`
加源文件列表参数（Task 1 Step 1 的 `runBuild(dir, logger, vrSources = {"main.n"})`，
一处改动、项目 TU 与库 TU 两个形状共用；轮 6 实测：光换入口名不够，Task 4 Step 8 的
「两个 `main()`」要一次 build 喂两份项目源）。

```cpp
//(1) D8：路径压过壳名。文件叫 zlib.n，壳里写 namespace alib ——
//    包名必须来自路径（zlib），不是壳（alib）。这是 owner 基规则的唯一直接证据。
static void TestPathBeatsShellName() {
    auto dir = scenarioDir("path_beats_shell");
    writeFiles(dir, {
        { "zlib.n",
          "namespace alib {\n"
          "int twice(int x) { return x * 2; }\n"
          "}\n" },
        { "main.n",
          "import zlib;\n"
          "int main() { return zlib.twice(3) - 6; }\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "zlib.n with a mismatched shell still builds");
    //(1b) 反向：用壳名限定必须失败，且诊断指名真正的前缀。
    auto dir2 = scenarioDir("path_beats_shell_neg");
    writeFiles(dir2, { { "zlib.n", "namespace alib {\n"
          "int twice(int x) { return x * 2; }\n}\n" },
        { "main.n", "import zlib;\n"
          "int main() { return alib.twice(3); }\n" } });
    CHECK(!compileDir(dir2), "the shell name is not a use-site qualifier");
    CHECK(compileLog(dir2).find("not imported") != std::string::npos,
          "rejecting alib.twice says the module is not imported");
}

//(2) 设计 §6 第二条：同一个包里两个同名类型必须是诊断，不是静默 first-wins。
//    **轮 6 实测：这条诊断今天就有，但不在产码侧**。同一 TU 写
//    `class Box { int a; }` ＋ `struct Box { int b; }`，ncc 给的是
//      Error: The field "struct Box" is conflicted with a exist field definition.
//      See also the definition of "class Box".
//    两条各测一份（`class`＋`struct`、`class`＋`class` 都是这个结果），来源是
//    `DuplicateFieldChecker::DetectConflict`
//    （`src/compiler/builder/DuplicateFieldChecker.hpp:196-220`：只有「两个都是
//    `NK_Function` 且不同裸池」才豁免，类型对保留名字相等即冲突）。库壳内的同名
//    走另一句（`SnMisc.cpp:76-83` 的 `The namespace member "%s" has has already been
//    defined.`，Task 5 Step 6b 把名词改成 package）。**所以 Step 6 不在 VM 侧再设
//    第二道重复键检查**（那是走不到、也解释不清的死代码，见 Step 6 的轮 6 改写）。
static void TestDuplicateTypeInOnePackageRejected() {
    auto dir = scenarioDir("dup_type");
    writeFiles(dir, { { "main.n",
        "class Box { public int a; }\n"
        "struct Box { int b; }\n"      //same unit, same name -> collision
        "int main() { return 0; }\n" } });
    CHECK(!compileDir(dir), "two same-named types in one package fail");
    CHECK(compileLog(dir).find("is conflicted with a exist field definition")
              != std::string::npos,
          "the reachable duplicate-type diagnostic still fires (measured today)");
    //(2b) 跨包同名不是错误——反向对照，防止键面改过头。
    auto dir2 = scenarioDir("dup_type_ok");
    writeFiles(dir2, { { "alib.n", "namespace alib {\nstruct Box { int a; }\n}\n" },
        { "main.n", "import alib;\nstruct Box { int b; }\n"
          "int main() { Box q; alib.Box p; return 0; }\n" } });
    CHECK(compileDir(dir2), "same type name in different packages is legal");
}

//(3) D10：字面量类型名歧义要报，唯一命中要能用表键推。
//    两份库各有一个 `struct S`，主 TU 同时 import ⇒ readStruct("S") 必须有歧义诊断；
//    只 import 一份 ⇒ 编译通过，且推到 VM 的串是限定名（轮 4 补全：原稿只留了签名）。
static void TestStreamLiteralResolvesAtCompileTime() {
    //(3a) 唯一命中：S 在 alib 里，常量池里推的是表键 alib.S，不是裸名。
    auto dir = scenarioDir("stream_literal_one");
    writeFiles(dir, {
        { "alib.n", "namespace alib {\nstruct S { int a; }\n}\n" },
        { "main.n", "import alib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 1; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"S\");\n"
          "  return r.a - 1;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "unique readStruct(\"S\") round-trips");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".nmod")).string());
    bool keyed = false;
    for (const auto& s : mod.stringConstants)
        if (s == "alib.S") keyed = true;
    CHECK(keyed, "the literal that reaches the VM is the qualified table key");
    bool bare = false;
    for (const auto& s : mod.stringConstants)
        if (s == "S") bare = true;
    CHECK(!bare, "no bare-name type literal survives");
    //(3b) 两个可见同名 ⇒ 歧义诊断，不是静默 first-wins（Step 9 的 hits>1 分支）。
    //    **轮 6 实测订正**：原稿这份正文是 `return bs.readStruct("S") == 0;`，
    //    而 `readStruct` 返回结构体、结构体不能与 `0` 比较——**这条自己就是个类型错误**，
    //    `!compileDir(dir2)` 会因为它而绿，歧义分支走没走到都不知道。现在这份与 `(3a)`
    //    **逐字同正文**，唯一的差别就是多一条 `import blib;`，正/负对照才成立。
    auto dir2 = scenarioDir("stream_literal_two");
    writeFiles(dir2, {
        { "alib.n", "namespace alib {\nstruct S { int a; }\n}\n" },
        { "blib.n", "namespace blib {\nstruct S { int b; }\n}\n" },
        { "main.n", "import alib;\nimport blib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 1; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"S\");\n"
          "  return r.a - 1;\n"
          "}\n" } });
    CHECK(!compileDir(dir2), "two visible types named S fail");
    CHECK(compileLog(dir2).find("ambiguous") != std::string::npos,
          "the ambiguity diagnostic is the one Step 9 adds");
    //(3c) 未命中：今天没有任何测试钉这条文案（Step 9 末尾实测），改判据时自己补。
    auto dir3 = scenarioDir("stream_literal_none");
    writeFiles(dir3, { { "main.n",
        "int main() {\n"
        "  ByteStream bs = new ByteStream();\n"
        "  return bs.readStruct(\"NoSuchType\");\n"
        "}\n" } });
    CHECK(!compileDir(dir3), "an unknown literal type name fails");
    CHECK(compileLog(dir3).find("type not found: NoSuchType")
              != std::string::npos,
          "the preserved bare wording is still the not-found path");
    //(3d) 轮 4 实测补：Step 9 的歧义文案让用户「qualify it (e.g. 'pkg.S')」，
    //    所以带点的字面量必须真的能用，否则那条建议在骗人。
    auto dir4 = scenarioDir("stream_literal_qualified");
    writeFiles(dir4, {
        { "alib.n", "namespace alib {\nstruct S { int a; }\n}\n" },
        { "blib.n", "namespace blib {\nstruct S { int b; }\n}\n" },
        { "main.n", "import alib;\nimport blib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 4; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"alib.S\");\n"
          "  return r.a - 4;\n"
          "}\n" } });
    CapturingIo cap4;
    CHECK(compileRun(dir4, cap4) == 0,
          "the ambiguous advice form (a dotted literal) round-trips");
}

//(4) Step 8：entryPoint 的写读往返。
static void TestEntryPointRoundTrip() {
    auto dir = scenarioDir("entry_round");
    writeFiles(dir, { { "main.n", "int main() { return 7; }\n" } });
    CHECK(compileDir(dir), "single-file program builds");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".nmod")).string());
    CHECK(mod.entryPoint >= 0, "entry point index recorded");
    CHECK(mod.entryPoint < static_cast<int32_t>(mod.functions.size()),
          "entry point index in range");
    //After ModuleLoader::Load, the index must still point at the entry:
    CHECK(mod.functions[mod.entryPoint].name == "main.main",
          "the entry key is the path-derived qualified name");
    //Library-only module (the compile entry is alib.n, not main.n):
    //no PROJECT unit declares main -> -1.
    auto dir2 = scenarioDir("entry_lib");
    writeFiles(dir2, { { "alib.n", kLibSource } });
    CHECK(compileDir(dir2, { "alib.n" }), "a library unit builds on its own");
    CompiledModule lib = ModuleLoader::Load(
        (dir2 / (dir2.filename().string() + ".nmod")).string());
    CHECK(lib.entryPoint == -1, "a library unit has no entry point");
}

//(5) 设计 §8：内置 Object 与用户 Object 在磁盘上是两个键。
//    轮 4 实测（ndisasm 直接看今天的类表）：`class Object { int marker; }` 写在根上，
//    表里同时有 `[0] Object (fields=0)`（内置）和 `[11] Object (fields=1)`（用户）——
//    **两个同名键**，而字节码 `new Object()` 编的是 `class=0`（内置那份 0 字段），
//    运行时 `x.marker = 5` ⇒ `NLang VM: struct field store out of bounds`。
//    所以这条用例钉的是「键名不再撞」，不是「用户 Object 可用」——后者卡在
//    编译器侧按裸名短路到内置单例的 5 个点上（`ExprResolverNew.cpp:120`、
//    `ExprResolverTypes.cpp:393`、`ExprResolverValues.cpp:46`、
//    `ExprResolverStdLib.cpp:59`、`DuplicateFieldChecker.hpp:279`），
//    本任务不改（见轮 4 报给用户的未排期项）。同一根因在库侧也实测到了：
//    `alib.Object y = new alib.Object(); y.tag = 1;` 编译通过（字段走的是用户那份），
//    但 `y.who()`（库里声明的方法）当场
//    `The function "who" does not exist or is not accessible`——所以 `(5)` 的 fixture
//    **只用字段，不用方法**，别把它写成正例去替那个短路 bug 背书。
static void TestBuiltinAndUserTypeNameCoexist() {
    auto dir = scenarioDir("object_key");
    writeFiles(dir, {
        { "alib.n", "namespace alib {\nclass Object { public int tag; }\n}\n" },
        { "main.n", "import alib;\n"
          "class Object { public int marker; }\n"   //§8 第三句：根级用户 Object
          "int main() {\n"
          "  alib.Object y = new alib.Object();\n"
          "  y.tag = 2;\n"
          "  return y.tag - 2;\n"
          "}\n" } });
    CHECK(compileDir(dir),
          "a library Object and a root Object build beside the builtin one");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".nmod")).string());
    int nBare = 0, nLib = 0, nRoot = 0;
    for (const auto& c : mod.classes) {
        if (c.name == "Object") ++nBare;       //内置：NO_OWNER ⇒ 裸名
        if (c.name == "alib.Object") ++nLib;   //库：路径派生限定名
        if (c.name == "main.Object") ++nRoot;  //项目根 TU：同样带包名（§8 第三句）
    }
    CHECK(nLib == 1, "the library declares exactly one alib.Object key");
    CHECK(nBare == 1, "the builtin keeps its bare Object key and nothing else shares it");
    CHECK(nRoot == 1, "the root-level user Object is keyed main.Object, not Object");
    //键分开了，指向的布局也必须分开：内置那份 0 字段，两份用户声明各 1 字段
    //（`CompiledClass::fieldCount`，include/nlang/vm/CompiledModule.h:279）。
    //这正是轮 4 实测「两个同名 `Object` 键、`new` 编到 0 字段那份」的形状。
    for (const auto& c : mod.classes) {
        if (c.name == "Object")
            CHECK(c.fieldCount == 0, "the bare key is the field-less builtin");
        if (c.name == "main.Object")
            CHECK(c.fieldCount == 1, "main.Object carries the user field");
    }
}
```
`(3)` 的 `writeStruct`／`ByteStream` fixture 形状照 `tests/e2e/bs_struct_class_simple.n`
（实测该文件就在，写法是 `ByteStream bs = new ByteStream(); ... S s2 = bs.readStruct("S");`）。
**(3a) 里 `alib.S w;` 是结构体局部变量，与那份 fixture 同形**（fixture 里 `S` 在根上，
这里 `S` 在库里 ⇒ 用 site 必须带包名，正是 R1 的规则）；别改成 `new S()`——结构体不走堆。
断言文案吃 `ambiguous`（Step 9 新增）与 `type not found: NoSuchType`（Step 9 保留的旧文案，
今天**没有任何测试钉它**，见 Step 9 末尾）。
`(5)` 的两条计数断言只吃 `mod.classes` 里的**名字**，不吃运行结果——运行面今天就是坏的
（上面那段实测），把它写成正例等于替编译器侧的短路 bug 背书。
五条都要注册进文件的 `main()` 调用串（这个文件是手写 `CHECK` 风格，**不是**自动发现，
与 `test_module_import.cpp` 的 QtTest 槽不同）。
D5（多段包里的 `native`）**不在本任务**：那条规则要等带点 `import` 落地才凑得出
「多段包的库源」这个前提，钉子排在 **Task 6 Step 1 的 `(5)`**（轮 4 补：原稿说了
「排在 Task 6 Step 1」，但 Task 6 Step 1 当时只有三条签名，并没有它）。

- [ ] **Step 2: 跑它确认失败**，并把观察到的错误数值（rb3 的串布局症状）记进提交说明。

- [ ] **Step 3: 缝搬到可共享处，VM 拿得到注册表**

`src/vm/CMakeLists.txt` 的 `target_include_directories(nlang_vm PRIVATE ...)` 追加
`${PROJECT_SOURCE_DIR}/src/compiler`（先例：`src/compiler/CMakeLists.txt:99` 反向列 `src/vm`）。

`VmBackend.h` 前向声明加 `class ModuleRegistry;`，公开段加：

```cpp
    //Phase 5: the package/qualified-name seam lives on the compile-time
    //registry. Codegen must build the SAME string the resolver shows the
    //user, so it borrows the registry (never owns it; it outlives codegen
    //inside one Build() call). The env pointer is the diagnostic channel:
    //VmBackend has no m_Env member (env arrives by value in Build() and by
    //reference in SaveModule()), so duplicate-key errors need it injected.
    void SetModuleRegistry(const ModuleRegistry* pRegistry,
        BuildEnvironment* pEnv)
    {
        m_pRegistry = pRegistry;
        m_pEnv = pEnv;
    }
```
私有成员加 `const ModuleRegistry* m_pRegistry = nullptr;` 与 `BuildEnvironment* m_pEnv = nullptr;`
（`VmBackend.h` 前向声明里 `class BuildEnvironment;` 若还没有就补）。

`ModuleBuilder.cpp:185-192` 的注入块里追加一行。轮 6 实测订正：**没有 `RegisterTypes` 这个
函数**（原稿那句是幻影），真实的类型入表顺序在 `VmBackend.cpp:46` 的 `GenerateStatements`
里，`GenerateTypes`（`:35-40`）本身是文档化的 no-op：

```
:47 RegisterBuiltinClasses()          ← 内建 Object/List/Dict 先占键
:51 MergeImportedClassesStructsArrays()← 库类型表并入
:52 RegisterStructs(root)              ← struct 名写入点（Step 6）
:53 RegisterClasses(root)              ← class 名写入点（Step 6）
```

而 `ModuleBuilder.cpp:193` 的 `backend->GenerateTypes` 在这次注入（`:185-192`）之后、
`:193` 之后的 `GenerateStatements` 之前 ⇒ 注入必然先于任何拼写需求：

```cpp
		vmBackend->SetModuleRegistry(&m_upEnv->Registry(), m_upEnv.get());
```

- [ ] **Step 4: `QualifiedFunctionName` 改缝，保留方法裸名**

`Register.cpp:18-41` 整体替换为：

> **放置位置（轮 2 实测）**：`Register.cpp:16` 开 `namespace {`、`:43` 才闭合，今天的
> `QualifiedFunctionName` 住在这个**匿名命名空间**里。把 `VmBackend::KeyOf` 的成员定义写进
> `:18-41` ＝ 把成员函数定义塞进嵌套匿名命名空间 → **非法，编不过**。两条新符号必须落在
> `:43` 的 `} // namespace` **之后**的文件级作用域（匿名 namespace 只留内部拼写函数）。
> 另外 `nlang::QualifiedName(...)` 这个自由函数没有对外头：本任务只在 `KeyOf` 里用它，
> 把 Interfaces 段里的「公开包装」改成「`Register.cpp` 内部符号」，别声称导出。

```cpp
//Canonical VM key of a declaration: "<package>.<name>", or the bare name
//when the node carries no owner tag. The package comes from the compile-
//time registry (path-derived), NOT from an AST namespace walk — a walk can
//only name a unit that literally wrote `namespace <path>`, and project
//members hang off the root, so every project key would stay bare.
//A CLASS/INTERFACE/ENUM METHOD always keeps a bare name, even when its
//owning type is package-nested: methods dispatch by name through their
//receiver (VmExecutor::FindMethodByName compares the bare name), so a
//package prefix would make every lookup fail.
std::string QualifiedName(const ModuleRegistry& reg, const SnField& field) {
	if (const SyntaxNode* pParent = field.Parent();
		pParent && (pParent->Kind() == NK_ClassDecl
			|| pParent->Kind() == NK_InterfaceDecl
			|| pParent->Kind() == NK_EnumDecl))
		return field.Name();
	return reg.QualifiedName(field);
}

//Codegen-side accessor: every name written into or looked up in the VM
//tables goes through here, so the spelling cannot drift from what the
//resolver reports. Requires the registry injected by ModuleBuilder.
std::string VmBackend::KeyOf(const SnField& field) const {
	assert(m_pRegistry && "VmBackend::KeyOf before SetModuleRegistry");
	return QualifiedName(*m_pRegistry, field);
}
```
**两个符号、两种可见性（轮 5 与 Interfaces 段对齐）**：
`std::string KeyOf(const SnField& field) const;` 声明进 **`VmBackend.h` 的公开段**（就紧贴在
Step 3 那条 `SetModuleRegistry` 之后，`:80-90` 区块内）——Step 5 的表里 `RegisterClass.cpp`／
`EmitCall.cpp`／`EmitExprNew.cpp` 等约十个文件调的就是它，不声明进头就编不过；
二参自由函数 `QualifiedName(const ModuleRegistry&, const SnField&)` **只留在 `Register.cpp`**
（`:43` 之后的文件级作用域），不进任何头。然后把调用点
`cf.name = QualifiedFunctionName(func)`（约 `:272`）改为 `cf.name = KeyOf(func)`。

- [ ] **Step 5: 类型表键与延迟名表**

按下表逐点替换（左＝今天，右＝改后）。每点都是一次表达式替换，不改控制流：

| 文件:行 | 今天 | 改为 |
|---|---|---|
| `Register.cpp:47` | `cs.name = sn.Name();` | `cs.name = KeyOf(sn);` |
| `Register.cpp:63` | `typeNames.push_back(fieldType->Name());` | `typeNames.push_back(KeyOf(*fieldType));` |
| `Register.cpp:86` | `FindStruct(typeNames[i])` | 不变（表里已是限定名，写入端与解析端同语言） |
| `Register.cpp:101` | `FindClass(typeNames[i])` | 同上 |
| `Register.cpp:122` | `FindStruct(pElemType->Name())` | `FindStruct(KeyOf(*pElemType))` |
| `RegisterClass.cpp:95` | `cc.name = sn.Name();` | `cc.name = KeyOf(sn);` |
| `RegisterClass.cpp:100` | `declMap[sn.Name()] = &sn;` | `declMap[cc.name] = &sn;`（键＝刚写入的限定名） |
| `RegisterClass.cpp:113/117` | `FindClass(ft->Name())`/`FindStruct(ft->Name())` | `FindClass(KeyOf(*ft))`/`FindStruct(KeyOf(*ft))` |
| `RegisterClass.cpp:157` | `declMap.find(cc.name)` | 不变（两端同为限定名） |
| `RegisterClass.cpp:161` | `FindClass(pDecl->SuperClass()->Name())` | `FindClass(KeyOf(*pDecl->SuperClass()))` |
| `RegisterClass.cpp:176` | `cc.name != "Object"` | 保留，注释改成「内置 `Object` 无 owner 标签 ⇒ 键仍是裸 `Object`」 |
| `RegisterClass.cpp:201` | `FindClass(sn.Name())` | `FindClass(KeyOf(sn))` |
| `EmitExprNew.cpp:55-56` | `FindClass(pClassDecl->BaseName())` | **改 `KeyOf(*pClassDecl)`**（轮 2 实测两行说法冲突：前置事实列它「必改」，本行原稿写「不变」）。事实：泛型只有内建三种（`ExprResolverTypes.cpp:88` 的 `List`/`Dict`/`Func`，用户类型不带实参＝D9），`BaseName()` 对内建实例返回裸名、对普通类返回声明名；内建声明无 owner 标签 ⇒ `KeyOf` 对它们**天然还是裸名**，对 `alib.Point` 给出 `alib.Point`。一条规则同时覆盖，不需要分支。命中失败是响亮的（`:57-63` 抛内部错误），Step 1 的 rb3 用例（`new alib.Point()`）就是它的正例。 |
| `EmitExprInitList.cpp:178/232` | `FindClass("List")`/`FindClass("Dict")` | 不变（内建，无 owner） |
| `EmitExprInitList.cpp:305-307` | `classDecl.BaseName().empty() ? classDecl.Name() : BaseName()` ＋ `FindClass(className)`（轮 4 实测：原稿写「元素类型名查表 → `KeyOf(*pElemType)`」是**错的**，`EmitInitListClassForm` 这个签名里没有 `pElemType`，它拿的是 `SnClassDecl&`） | `FindClass(KeyOf(classDecl))`——与 `EmitExprNew.cpp:55-56` 同一条规则（内建擦除实例无 owner ⇒ 仍裸名），命中失败是响亮的（`:308-312` 抛 `init-list class not registered: <名字>`） |
| `EmitStmtSwitchTry.cpp:312-316` | `find_if(c.name == ccName)`（手工扫表，`Find*` 的 grep 看不见它） | `ccName` 改由 `KeyOf(*pType)` 产出（catch 类型声明在 AST 上，`:310` 手上就是 `SnField*`） |
| `EmitStmtSwitchTry.cpp:428` | `FindClass(pParent->Name())`（`Access(SnSuperCallStmt&)` 里找父类构造） | `FindClass(KeyOf(*pParent))`——轮 2 实测：前置事实已把 `:428` 列进「必改」，原稿这张表只有 `:312-316` 一行，读表的人会以为整个文件只有一处 |
| `TypeDesc.cpp:179`＋`:229` | `mod.FindStruct(pType->Name())`／`mod.FindClass(pClass->Name())` | 名字改走 `KeyOf`。**但 `BuildTypeDesc` 是自由函数**（`src/vm/TypeDesc.cpp:133`，声明 `include/nlang/vm/TypeDesc.h:104`），手上只有 `const CompiledModule&`，拿不到 `VmBackend::KeyOf` ⇒ 签名加一个 `const ModuleRegistry&`（Task 3 的 `QualifiedName` 就是它的公开入口），四个调用点 `Register.cpp:114`、`RegisterClass.cpp:138`、`VmBackend.cpp:169/173` 传 `*m_pRegistry`，四处递归 `:165/211/222/223` 转发。`BaseName()=="List"/"Dict"` 的比较在 `:203`／`:214`，**保持裸名**（内建擦除键，原稿把它们当成查表点列进 179/203/214/229 是错的） |
| `VmExecutorOpsObjects.cpp:321-330` | `ReceiverClassIndex` 里 `classes[i].name == "Object"` | **代码不改**，加注释：限定后只有无 owner 的内建 `Object` 键是裸 `Object`，用户 `pkg.Object` 天然不命中——这正是想要的语义（见 Step 9 的内建判定） |
| `VmExecutor.cpp:26/32/38-47` | 内建类型名 | 不变（内建无 owner） |
| **`EmitCall.cpp:114`** | `FindStruct(pFormalType->Name())` | `FindStruct(KeyOf(*pFormalType))`（轮 1 补） |
| **`EmitExprCast.cpp:279`** | `FindClass(targetType->Name())` | `FindClass(KeyOf(*targetType))` |
| **`EmitExprInitList.cpp:131`** | `FindStruct(pElemField->Name())` | `KeyOf` |
| **`EmitExprInitList.cpp:345`** | `FindStruct(structName)` | 吃的是本文件产码写下的串：改产码端为 `KeyOf`，这里跟随后不动 |
| **`EmitStmtAssign.cpp:62/311`** | `FindStruct(varType->Name())`／字段型查表 | `KeyOf`（两处分别核：`:62` 吃 AST 型，`:311` 吃 `m_structFieldTypeNames` 里存的名字——那条链的写入端在 `Register.cpp:58-66`，两处必须同一规则） |
| **`EmitStmtDecl.cpp:35`** | `FindStruct(evalType->Name())` | `KeyOf` |
| **`EmitStmtForeach.cpp:251`** | `FindStruct(...)` | `KeyOf` |
| **`EmitStmtStore.cpp:149`** | `FindStruct(elemType->Name())` | `KeyOf` |
| **`Register.cpp:122/126`**（`RegisterArrayType`，函数体 `:118-126`） | 数组元素型查表 | `KeyOf`（吃 `pElemType->Name()`，是 AST 型）。**轮 7 把 `:86/101` 从这一行删掉**：那两处吃 `typeNames[i]`，写入端 `:63` 已是 `KeyOf`，消费端一行不动（上面第 1536/1537 行的「不变」才是对的；这一行原来把四个点并列，等于在同一张表里给了自己两个相反的答案） |
| **`RegisterClass.cpp:173-179`＋`VmExecutorOpsObjects.cpp:321-330`＋`BuiltinNames.h:28-31`** | 内建 `Object` 的三处**按裸名**判定：`ApplyImplicitObjectInheritance` 给没有父类的类补 `Object`（`cc.name != "Object"`）、`ReceiverClassIndex` 找 boxed 接收者的 `Object` 类槽、编译器 `IsBuiltinClassName` 白名单 | **三处都不改判定式，各加一行注释**钉住「裸名＝无 owner 的内建」（轮 1 的原稿这一行写的是 `InitStructHeap`＋`FindEnum`＋`GetOrMatchStruct`/`g_lastCaller`——轮 2 实测**这些符号在本仓库不存在**：`InitStructHeap` 在 `src/vm/VmExecutor.cpp:69-80`，只清堆与调用栈，一个名字都不查；`FindEnum` 全仓库零命中）。行为核对：限定后用户类 `pkg.Object` 的键带包名，`cc.name != "Object"` 为真 ⇒ 它照样被补 implicit `Object` 父类（正确：它不是内建根），`ReceiverClassIndex` 不再把它当内建根（正确）。**Step 9 的用例集合里必须有一条「库里声明一个叫 `Object` 的类」**，否则这三处注释就是空话 |
| **`VmExecutorSer.h:204-206` ↔ `:290-304`** | 对象流写 `cc.name`／读回 `FindClass(className)` | **代码不改，语义变**：写出的名字随表键变成限定名，读回端吃的就是自己写出的串，所以同一次编译内自洽；跨程序（旧 ncc 写、新 nvm 读）不做兼容读（D2）。这里加一行注释钉住「写读同源」，并加一条 blob 往返断言（Step 1 的正例里加） |
| **`IntrinsicsByteStream.cpp:208/256`、`IntrinsicsFileStream.cpp:220/270`** | `FindStruct(typeName)`／`FindClass(declaredName)` | 同上，吃字节码里自己写的串 → 不改，加写读同源注释 |

改完后 `ForEachDeclNode` 的每个 decl 都能拿到包名，因为标签仍在 root 成员上（前置事实）。
**这张表的闭合性用命令核，不靠记忆**（轮 2 实测：原稿这条命令带 `grep -v 'int Find'`，
数出来是 **35** 不是 38——38 是把 `CompiledModule.h:307/314/321` 三个定义也算进去了）：

```bash
grep -rn 'Find\(Struct\|Class\|Function\)(' src/vm include/nlang/vm | grep -v 'int Find' | wc -l
grep -rn '\.name = \|\.name ==\|name != ' src/vm include/nlang/vm src/tools/ndisasm/main.cpp | wc -l
```
第一条＝ **35**（读取端），第二条＝ **18**（写入端＋手工扫表，`Find*(` 看不见它们）。
两条的输出逐行归入「改 `KeyOf`／保持裸名／写读同源」三类之一，一行都不许漏；
表里没列到的行要么是本任务不碰的（`VmBackend.cpp:32` 的模块名、`CompiledModule.h`
三个 finder 的函数体），要么就是漏了。**别把这里的 35 和 `test_library_index.cpp:47`
的 `size() == 38`（符号索引条数）混成一个数。**

- [ ] **Step 6: 类型表的重复键——**实测走到不了，本步不加检查（轮 6 改写）****

**原稿在这一步加一份「扫 `m_compiledModule.classes` 找同名键 ⇒
`Duplicate type name '%s' in one module.`」的诊断，并让 Step 1b 的 `(2)` 去钉它。轮 6 实测
两条都落不了地，现在改成「不加」并把证据写进注释与提交说明**：

1. **同包同名类型在编译期就被拦住，产码看不到**。实测（`build-dev/tests/Release/ncc.exe`，
   `class Box { int a; }` ＋ `struct Box { int b; }` 同一 TU，以及 `class`＋`class` 另一份）
   给出的都是 `DuplicateFieldChecker::DetectConflict`
   （`src/compiler/builder/DuplicateFieldChecker.hpp:196-220`）那句
   `The field "struct Box" is conflicted with a exist field definition.` ＋
   `See also the definition of "class Box".`，编译当场失败；库壳内的同名走
   `SnNamespace::MergeFrom`（`src/compiler/SnMisc.cpp:76-83`）的
   `The namespace member "%s" has has already been defined.`。
   豁免只给「两个都是 `NK_Function` 且不同裸池」，**类型对没有豁免**。
2. **跨模块同键是「同一个类型」，不是错误**。`Import.cpp:236-245` 的
   `MergeImportedTypeTables` 按名字 dedup（Step 7 把它写成事实注释），两份 `alib.Box`
   本来就该合成一条，报重复反而是错的。
3. 就算要扫，原稿那份循环只扫 `m_compiledModule.classes`，而 `CompiledModule.h:299-300`
   把 `structs`／`classes` 分成两个表——`class Box` ＋ `struct Box` 这种组合它压根比不到。

所以本步的**交付物**是：在两个写入点各加**一行注释**——`class` 的名字写在
`RegisterClass.cpp:93-102`（`cc.name = sn.Name();` 在 `:95`），`struct` 的名字写在
`Register.cpp:45-71` 的 `RegisterStructDecl`（`cs.name = sn.Name();` 在 `:47`，
`m_compiledModule.structs.push_back` 在 `:68-69`；`RegisterStructs(SnNamespace& root)` 是
`:73` 起的那个遍历器，`ForEachDeclNode` 在 `:74`——轮 3 实测过这批行号，别退回
「`:94` 自己遍历」／「`:108-113` 是 struct 入表」那两个错号，`:108-113` 是 v1.12 类型描述符
注释＋`BuildTypeDesc` 循环）——注释内容：「同包重名由编译期的 `DuplicateFieldChecker` 拦下，
类型表在这里不设第二道闸；跨模块同键＝同一类型（见 `Import.cpp:236-245`）」。设计 §6 第二条的钉子挪到 Step 1b 的 `(2)`，吃的是**真实那句**
文案（`is conflicted with a exist field definition`）。
**函数侧本来就不加**（轮 1 纠正）：`SnMisc.cpp:88-115` 说明同名跨模块函数是合法重载，
入表查重会误杀 `tests/e2e/default_overload_basic.n`／`default_overload_ambig.n`。

`VmBackend` **没有** `m_Env` 成员（`VmBackend.h` 只有 `SaveModule(BuildEnvironment&)` 形参，
`Build()` 按值拿 env）→ Step 3 注入的 `BuildEnvironment* m_pEnv = nullptr;` **保留**，
它的消费方是 Step 8 的「两个入口候选」诊断（那里是真的要发编译错误），不再是本步。
写入点的判空注释别写进 `Register.cpp`——本步不发诊断。

- [ ] **Step 7: 导入去重按限定名，注释里的「特性」删掉**

`Import.cpp:236-245`：`im.classes[i].name` 已是限定名（生产它的那次编译用同一个缝），所以
`FindClass(im.classes[i].name)` 的语义自动变成「限定名相同才合并」。把 `:240` 的
`// dedup to existing (e.g. user Object / built-in)` 改成事实：
`// same qualified name == same type (a distinct package makes it distinct)`。
`MergeImportedTypeTables` 的 struct 分支同理。

- [ ] **Step 8: `entryPoint` ＋ 格式 13 ＋ 执行入口**

`include/nlang/vm/CompiledModule.h`：
- `:43` → `inline constexpr uint16_t NMOD_FORMAT_MINOR = 13;`，并在上面 v1.12 段落之后补
  一段 v1.13 说明（限定名入表＋`entryPoint` 字段＝**布局**变更）。
- `CompiledModule` 的 `std::string name;` 之后加：
  ```cpp
  //v1.13: index of the entry function, -1 when the module exports none.
  //By-name lookup is gone: `main.n` in a directory is package `main`, so
  //its entry key is `main.main`, and the name alone no longer identifies it.
  int32_t entryPoint = -1;
  ```

`ModuleSaver.cpp:44-47` 模块名写出之后紧跟：
```cpp
    fs.write(reinterpret_cast<const char*>(&mod.entryPoint),
             sizeof(mod.entryPoint));
```
`ModuleLoader.cpp`：`:67` 的 `minorVer < 12` → `minorVer < NMOD_FORMAT_MINOR`，并在读回
`mod.name` 之后（`:83` 附近）加对应的 `entryPoint` 读取＋ `fs.good()` 检查。

写入端：`VmBackend` 产码收口处填 `m_compiledModule.entryPoint`。**收口点＝`GenerateAllBytecode`
（`src/vm/backend/VmBackend.cpp:66-69`）的末尾**（轮 1 实测：`SaveModule` 在 `:454`，放在它里面
`nvm`／`ndisasm` 拿到的内存态 `CompiledModule` 就没有入口；`GenerateStatements`（`:46`）只跑
语句阶段，函数表还没定完整）。

**入口的判定规则（轮 2 补：原稿写「owner＝入口 TU」，但本仓库没有「入口 TU」这个概念——
`BuildEnvironment`／`BuildParams` 里没有一个 `mainFile` 字段，ncc 收的是源文件清单，
今天全靠 `VmExecutor.cpp:89` 的按名查表挑入口）**。可实现的规则只有 AST＋注册表两个信息源：

```cpp
//Entry = a root-level (non-method) `main` declared by a PROJECT unit.
//Libraries never provide the entry; a module with zero candidates keeps
//entryPoint = -1 and nvm reports "no entry point" instead of today's
//"no 'main' function found". Two candidates is a build error, not a
//first-wins tie-break: the keys are `a.main` and `b.main`, both legal,
//so only the source paths can tell the user which file they meant.
```
遍历 `GenerateAllBytecode(SnNamespace& root)` 手上那份 root：`Name()=="main"`、
`Kind()==NK_Function`、父节点不是 `NK_ClassDecl`/`NK_InterfaceDecl`/`NK_EnumDecl`
（方法一律裸名，方法 `main` 不是入口），且 `m_pRegistry->OwnerOf(member)` 指向的模块
`isLibrary` 为假。命中 0 条 ⇒ `-1`；命中 1 条 ⇒ `KeyOf` 之后在函数表里查索引；
命中 ≥2 条 ⇒ 经 `m_pEnv`（Step 3 注入的那条通道）报编译错误，**文案带上两个源文件路径**
（`ModuleRegistry` 里查 `ModulePathOf`／`DirectoryOf`，`src/compiler/builder/ModuleRegistry.h:99/:108`）。
`CompiledModuleNodeBuilder.hpp:425` 的 `if (cf.name == "main")` 改判 `KeyOf` 结果。
**这条 ≥2 的分支要有测试**（轮 2：设计 §4 的「多个 TU 各有 `main()`」原本没排进任何任务；
轮 6 实测：原稿的 `a.n`／`b.n` 同目录形状**根本到不了这条分支**）。

> **轮 6 实测（两份 `main` 到底怎么写才命中 ≥2）**
>
> - 同目录两份源：`tm.nproj` ＋ `a.n`／`b.n` 各一个 `int main()` ⇒ 编译期先被**既有的重名
>   检查**拦掉：`The field "Int32 main()" is conflicted with a exist field definition.`
>   ＋ `See also the definition of "Int32 main()".`（`DuplicateFieldChecker`；两根源都
>   `DirectoryOf`＝`""` ⇒ `ShareBarePool` 为真，`src/compiler/builder/ModuleRegistry.h:124-131`）。
>   拿它当入口诊断的用例＝测错了门，Step 8 的分支一次都没执行。
> - **不同目录**两份源：`x/main.n`（`return 1`）＋ `y/main.n`（`return 2`）⇒ 今天
>   **安静编过**（`Compiled successfully`），`nvm` 跑出来 `rc=1`，即第一份注册的 `main`
>   赢了、第二份是死代码。这才是 Step 8 那条诊断要接管的确切形状。
> - 这个形状对 harness 有一个硬要求：**`m_sProjectDir` 必须指向场景根**，否则
>   `DeriveModulePath`（`ModuleRegistry.cpp:48-65`）对两份源都回落到文件 stem ⇒ 两个模块
>   路径都是 `main`、`DirectoryOf` 都是 `""` ⇒ 又落回上面那条重名诊断。所以 Task 1 的
>   `runBuild` 里加 `params.m_sProjectDir = dir.string();`。这一行对现有全部扁平场景是
>   **行为不变**的（`main.n` 相对 `dir` ＝ `main.n`，`DotifyModulePath`（`:23-30`）去掉
>   `.n` 后照样得到 `main`），只是顺手让嵌套目录在进程内可测。
> - CLI 侧不需要新能力：`ncc -p <nproj>` 已经是多源入口（`src/tools/ncc/main.cpp:279-284`，
>   `m_sProjectDir = project.projectDir`），上面的实测就是走它做的。

```cpp
//Design §4 "two TUs each writing main()": different directories, because same
//directory is already rejected by the existing duplicate-name check (round 6
//measurement) - that guard would swallow this case before the entry scan runs.
static void TestTwoMainsRejected() {
    auto dir = scenarioDir("two_mains");
    writeFiles(dir, {
        { "x/main.n", "int main() { return 1; }\n" },
        { "y/main.n", "int main() { return 2; }\n" } });
    CHECK(!compileDir(dir, { "x/main.n", "y/main.n" }),
          "two project entry candidates are a build error");
    const std::string log = compileLog(dir, { "x/main.n", "y/main.n" });
    CHECK(log.find("x.main") != std::string::npos
          && log.find("y.main") != std::string::npos,
          "the diagnostic names both candidates by their qualified key");
}
```
文案带的是**两条限定键**（`x.main`／`y.main`）而不是文件系统路径：闭合命名模型里键＝路径
派生（Step 4/5 之后 `KeyOf` 就是这么拼的），一一对应用户想写的那个文件，而 `a.n` 这种
磁盘名字反而在跨根场景下说不清是谁。断言因此吃 `x.main`／`y.main`。
`writeFiles` 现在**不**建子目录（`tests/test_vm/test_library_source.cpp:59-64`：只有
`std::ofstream out(dir / entry.first)`，而 `scenarioDir` 只 `create_directories` 了场景根本身），
所以 `{"x/main.n", …}` 这份写下去会静默失败（`ofstream` 打不开不存在的 `x/`，`writeFiles`
返回 `void` 没人看）。本步给它的循环体补一行：

```cpp
void writeFiles(const fs::path& dir, const Files& files) {
    for (const auto& entry : files) {
        fs::path target = dir / entry.first;
        fs::create_directories(target.parent_path());   //nested fixtures need parents
        std::ofstream out(target, std::ios::binary);
        out << entry.second;
    }
}
```
（Task 6 用的是另一份脚手架 `tests/test_vm/test_thirdparty.cpp` 的 `packageDir()`＋显式
`fs::create_directories(pkg / "vendor")`，`:93-123` 的 `buildAndRun`，与本处无关，别去找
`writeFiles`。）注册进 `main()` 调用串，与 `TestEntryPointRoundTrip` 同一处。

**`isNative` 分支保留**（轮 1 纠正：编译器确实写它——`VmBackend.cpp:133-136` → `:182-190` 的
`FillNativeFunctionRecord` 置真，`Import.cpp:383` 转发；原稿「编译器从不写」的说法作废），
但入口是 native 时给**编译期诊断**而不是静默 `CallNative`，文案里带上限定键。
`VmExecutor.cpp:89-91` 替换：
```cpp
    if (module.entryPoint < 0
        || module.entryPoint >= static_cast<int32_t>(module.functions.size()))
        throw std::runtime_error("NLang VM: module has no entry point");
    int mainIdx = module.entryPoint;
```
**导入模块的 `entryPoint` 不参与任何事**（轮 3 勘误：原稿说「`ModuleLoader:124-136` 为导入
模块建的 stub 保持 `-1`」——那条区间实际是 `intrinsicId`（`:123-126`）与 `nativeFlag`
（`:128-133`）的读取，`ModuleLoader.cpp` 里根本没有「为导入模块建 stub」的函数，`:63`
那句 "stub reconstruction" 讲的是 v1.11 拒收，不是导入路径）。真实机制：
`Import.cpp:362` 起把 `im.functions[i]` **逐字段**拷进 `placeholder`（`:374-394`），
`entryPoint` 是 `CompiledModule` 的模块级字段、不在那份清单里，所以拷贝路径天然不涉及它。
要写的注释因此只有一句：`entryPoint` 只有**根模块**那份会被 `VmExecutor` 读；库 `.nmod` 里
它是 `-1`（Step 8 的规则已经保证：库单元的 `main` 不算入口候选），而且即使非 `-1`
也不可信——合并之后函数表索引会平移。**不要**为导入模块去填这个字段，也别加回落。

`tests/packaging/verify_package.py:51` 的 `12` → `13`。

**版本门的两面都要动（轮 2 实测：原稿只写了地板一面，本任务自己的 ctest 门会红）**：

1. `ModuleLoader.cpp:67` 地板 `minorVer < 12` → `minorVer < NMOD_FORMAT_MINOR`。
   天花板 `:71` 的 `minorVer > kCurrentMinorVer` 已经是常量驱动（`kCurrentMinorVer`
   在 `:59` ＝ `NMOD_FORMAT_MINOR`），**这两条是两个独立的 `if`，不是一条**（原稿曾合并描述）。
2. `ModuleLoader.cpp` 里还有**四处按版本门控的读取**：`:219`、`:288`、`:347`、`:376` 的
   `if (minorVer >= 12)`（v1.12 类型描述符）。地板抬到 13 之后这些条件**恒真**＝死代码。
   按「不考虑向下兼容」的既定规则：把这四个 `if` 拆掉、保留块内语句，并把
   `:59-66` 那段讲 v1.11 的注释改写成 v1.13 的事实（布局变更＝限定名入表＋`entryPoint`）。
   `:124/:128/:178/:206/:411` 那些 `>= 1/6/5/9/2` 的门**同样恒真**，但它们不属于本任务的
   布局面，**本步不动**，避免把一次格式升级写成全文件重构（记进阶段 6 的清理面）。
3. `tests/test_vm/test_debugger.cpp` 有**三处**钉死了当前版本号，Task 4 一改常量就红：
   `:254`（`CHECK(bytes[10] == 0x0C …  "fresh module should be stamped minorVer 12")`）、
   `:300`、`:348`（同形）。三处的 `0x0C` → `0x0D`、文案 `minorVer 12` → `minorVer 13`，
   它们下面各有一行把版本改成更旧的字节做负控（`:257` 的 `= 0x09`、`:303` 的 `= 0x0A`、
   `:351` 的 `= 0x0B`）——这三条仍然有效（都比 13 旧），**不用改值**，但要把上界的
   反向用例补上（下一步）。
4. **补一条边界负例**（轮 2：今天只有「太旧」，没有「太新」的钉）：在同一文件加
   `test_loader_accepts_ceiling_and_floor()`（**轮 5 按本文件真实约定改名并补注册步骤**：
   `test_debugger.cpp` 的用例是 `test_*` 小写下划线的**自由函数**，不是 `Test*` 类方法，
   而且**没有自动发现**——`main()` 从 `:2213` 起逐条手写调用，`:2222-2225` 就是
   `test_v19_loader_rejects_v1_8(); test_loader_rejects_v1_9(); test_loader_rejects_v1_10();
   test_loader_rejects_v1_11();` 四条注册。**漏注册＝这条负例从未跑过而门照样绿**），
   做法照抄 `:240-263` 的读字节→改 `bytes[10]`／`bytes[11]`→写到 `scratchDir()` 新文件→
   `try { ModuleLoader::Load(...) } catch`→断言 `what` 里那句话：
   把 `bytes[10]` 打成 `0x0E`（14）断言加载失败且消息含
   `was written by a newer ncc; upgrade ncc/nvm to run it`（`ModuleLoader.cpp:71-75` 的天花板
   分支原文），再打成 `0x0C`（12）断言含 `is outdated; recompile with current ncc`
   （`:67-70`）。两条都断言文案而不是只看抛异常，理由同 Task 1 Step 1 那条。
   注册行加在 `:2225` 的 `test_loader_rejects_v1_11();` 之后。
   **顺带**：上一条列的三处锚点（`:254-256`／`:300-302`／`:348-350`）各自带一句「别把补丁
   打到别的字段上」的守卫注释，`:253` 那句写的是 `fresh build must carry the current minor 12`，
   常量抬到 13 后注释与断言文案一起改，别让注释继续讲 12（Step 11 之前的 `git diff` 里逐条看过）。
5. **文档里的格式号同步（本步负责，不等 Task 8）**（轮 5 实测：Task 8 的盘点门是
   `grep -rn "namespace" docs/*`，**扫不到不含 `namespace` 这个词的格式号**，所以
   `module-serialization.md` 的三处 12 会一路错到 Task 8 之后）：
   - `docs/user_manual/en/vm-architecture/module-serialization.md:15` `uint16 minorVer = 12` → `= 13`
   - `docs/user_manual/en/vm-architecture/module-serialization.md:23` 版本历史段，在 v1.12 之后**加** v1.13 条目
   - `docs/user_manual/en/vm-architecture/module-serialization.md:34` 「the loader refuses minor < 12 outright」→ `< 13`
   - `docs/user_manual/zh/vm-architecture/module-serialization.md:13`／`:21`／`:28` 同形三条（成对改）
   合计 **6 处**（每语言 3 处）。Task 8 的新页只讲语义，不改这三处。

- [ ] **Step 8b: 调试／回溯面的名字（D13，本阶段唯一一处用户可见文案变化）**

**规则**：表键就是显示名，**不做第二套短名**——`FormatBacktrace`、`VmExecutorDebug.cpp:76`
（`info.funcName = frame.func->name`）、`DebugSessionController.cpp:145/152`（函数名断点匹配）、
nide 的调用栈一律跟着限定名走。理由：短名要给每个键再配一份「怎么缩短」的规则，就是给同一个
概念第二个真相源，与已定的命名模型冲突。

代价（轮 1 实测，全是原稿没列的红）：`main.n` 场景的入口键是 `main.main`。**轮 4 把这一面
拆成两半，因为它们卡在不同的门上**：

- **8b-机械（表键查串，不受 D13 批准影响，必须与本提交一起做）**：测试代码里
  `FindFunction("…")`／`AddFunctionBreakpoint("…")`／`hooks.target = "…"` 这些串**就是表键**，
  键一变限定，不改就编译不过或断言必红，与「用户看到什么文案」无关。
  `tests/test_vm/test_debugger.cpp` 的完整名单实测 **14 处**：`FindFunction("main")`
  `:858/:892/:929/:973/:1273/:1766`、`FindFunction("triple")` `:184`、`hooks.target`
  `:680`（`"inner"`）/`:756`/`:795`（`"main"`）、`AddFunctionBreakpoint("inner")`
  `:1432/:1518/:1520`、`AddFunctionBreakpoint("val")` `:1475`；
  `tests/test_nide/test_debug_client.cpp:162/178`、`tests/test_nide/test_mainwindow.cpp:2284/2556`。
  闭合命令（行数必须 14）：
  `grep -n 'FindFunction("\|AddFunctionBreakpoint("\|hooks.target = "' tests/test_vm/test_debugger.cpp | wc -l`
- **8b-可见（D13 的人眼文案，需要用户点头，见 Step 8b′）**：回溯打印串、ndb 的
  `b main` → `b main.main`、`b print` → `b <包名>.print`、`ndisasm -func <name>` 的过滤键、
  e2e 金样本与文档示例里的展示拼写。

`src/tools/ndb/MachineFrontEnd.cpp` 的真实分工轮 4 实测纠正：`:118-133`（`DoBreakpoint`）
按 `AddBreakpoint(file, line)` 走，**不按名扫表**；按名扫表只在 `:135-158`（`DoBreakFunction`，
`:144-145` 的 `func.name != name`）。`src/tools/ndb/DebugSession.cpp:236`
（`f.name == arg`）／`DebugSessionController.cpp:152` 的匹配源就是表键，不用另改。

**金样本要跟着改的清单（轮 3 实测，全在 `tests/e2e/manifest.txt`）**：`:1021`
`Breakpoint 1, main (dbg_break_continue.n:9)`、`:1022` `Stopped: inner (…)
`、`:1025` `Breakpoint 1, triple (mathutil.n:3)`、`:1026` `Breakpoint 1, main (…)`、
`:1029` `Throw: main (…)` 五条的期望串里嵌函数名，限定化后逐条变长；
`ndisasm -func <name>` 的过滤键（`src/tools/ndisasm/main.cpp:146`）也是同一套拼写，
轮 6 把这一面**结掉了**：manifest 里确实没有 `-func` 用例，`grep -rn ndisasm docs/` 也只有
用法概要 `ndisasm -func <name> <module.nmod>`（en `cli-tools/ndisasm.md:11`／zh `:9`，
`<name>` 是**占位符**不是拼写）⇒ **文档零行可改**，原来挂在 Task 8 名下的那条查证到此为空；
真正缺的是门，见下面的 Step 8c。

- [ ] **Step 8b′: D13 的用户确认——本任务的前置条件，不是中途阻塞点（轮 4 纠正）**

原稿把确认写在任务中间（「其他步骤先做完把 diff 留在工作树里等确认」），实测会把 Task 4
逼进死角：8b-机械那半边必须与键限定同提交，否则 Step 11 的 ctest 红；而按原稿措辞
「Step 8b 的文件一律不改」会把 `test_debugger.cpp` 的 14 处也扣住不放，Step 12 提交完
Step 13 的 `git status --porcelain` 又必然在基线行 `?? main.n` 之外多出条目（轮 7 把措辞改准：
本工作树里这条命令永远非空，写「必然非空」等于没写）。所以顺序改成：

**开始 Task 4 之前**，D13（`at main.main`、`b main.main`、`ndisasm -func main.main` 这三例
前后对照）必须已经从用户处拿到明确答复。没拿到就不要进本任务——把 Task 1～3 做完后停下。
本步不留「先做完再把 diff 挂起等确认」的中间态：那既过不了门，也违反「一个提交一个可测结论」。
答复原文与影响面写进 Step 12 的提交说明。文档页（Task 8）必须写明这条拼写变化。

- [ ] **Step 8c: ndisasm 的显示面——D13 唯一一处今天零门禁的用户可见输出（轮 6 补）**

**实测的缺口**（轮 6 立、轮 7 重测行数的命令是**不带 `--include` 的**
`grep -rn ndisasm tests/` ＝ **4 行**：`tests/CMakeLists.txt:859-862` 的 `version_ndisasm` ＋
`:867` 的 TIMEOUT 组 ＋ `tests/packaging/verify_package.py:54`。轮 6 记的「3 行」是我给命令
加了 `--include=*.txt --include=*.cpp --include=*.cmake` 之类过滤把 `*.py` 挡掉的后果——
**加过滤的 grep 不能当收口证据**。第 4 行不改变结论：`verify_package.py:54` 只是 `BIN_FILES`
里的 `'ndisasm.exe'` 一个字符串，而 `BIN_FILES` 的使用点在 `:188-201`，逐条 `bin/<name>`
**存在性／布局检查**，从不执行它。也就是说 ndisasm 全生命周期只被跑过一次 `--version`，
**没有任何一条用例把它对着一个 `.nmod` 跑过**）：
`src/tools/ndisasm/main.cpp` 打印的**就是表键本身**——`:28` `func.name`、`:93` `st.name`、
`:114` `cc.name`，而 `:146` 的 `-func <name>` 过滤是 **拿用户输入的字符串和表键精确比对**
（`func.name != funcFilter`）。D13 说「表键就是显示名、不做第二套短名」，那么这一面既是
输出又是**输入**：键限定化之后 `ndisasm -func main` 会一个都不匹配，而用户看到的只是
「少了一段输出」，退出码照常 0。没有门，这个错就永久静默。

**基线实测**（`temp/rb9/` 现编现跑，`ncc build -p tests/e2e/proj_same_dir/….nproj`
＋ `ndisasm` 全量 64 行 `function` 记录——这个 64 是**函数条数**，与 ctest 门限 64 无关，
别把两处的数当同一条线索）——动手前照着这份数对，别凭印象：
- `… function main (… file=…\proj_same_dir\main.n)`／`… function f (… file=…\extra.n)`
  ＝ 今天**裸名**的两条项目模块自由函数 ⇒ 本任务后变 `main.main`／`extra.f`。
  这是本步那条门的**全部理由**。
- `… function io.print (… native, file=…\stdlib/io.n)` ＝ 今天就带点，Task 4 后**不变**
  （D12：外壳没了但包名还是 `io`）。留着当对照，证明门不是「见点就算对」。
- **57 条 intrinsic 方法记录全是裸名**，而且**本来就是重复的**：`writeInt`/`readInt`/
  `toString`/`get`/`set` 各 2～3 条（`ByteStream`、`FileStream`、`List`、`Dict` 各一份）。
  方法按已定规矩**永远保持裸名**（靠接收者分派），所以本任务**不碰它们**，表里会同时存在
  限定名与裸名两种形状——这不是没改干净，是设计本身。**因此新门绝不能用方法名当过滤键**：
  `ndisasm -func length` 今天就会打印 3 条，拿它做断言等于把一条既有歧义钉成规范。

**要加的门（就一条 ctest 用例，真编译真跑，不 mock）**：

```cmake
add_test(NAME ndisasm_qualified_func_name
    COMMAND $<TARGET_FILE:ndisasm> -func extra.f
        ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.nmod)
set_tests_properties(ndisasm_qualified_func_name PROPERTIES
    DEPENDS proj_same_dir_compile
    PASS_REGULAR_EXPRESSION "function extra\\.f ")
```
挂在**已有**的 `proj_same_dir_compile`（`tests/CMakeLists.txt:966-969` 写出的
`${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.nmod`，注意是 `CMAKE_BINARY_DIR` 不是
`CURRENT_`，照抄那两行别改）上，不新增 fixture、不新增编译产物；插入位置就贴在
`set_tests_properties(proj_same_dir_run …)`（`:972-974`）之后、与 `proj_*` 组同一段，
并把新条目名加进 `tests/CMakeLists.txt:1032-1039` 那条
`set_tests_properties(… PROPERTIES TIMEOUT 180)` 的名单（`proj_same_dir_compile` 自己就在
里面；这一段上面的注释就是理由：项目模式的条目是「真编译」，3 分钟的单元测试上限对它们
太紧）。
`-func` 命中与不命中的退出码都是 0，所以断言必须落在**输出串**上；末尾那个空格把
`extra.f` 与假想的 `extra.fx` 分开。
**它的失败侧不在本步**（轮 6 说清，别为了看红去 revert 已提交的代码）：Step 8c 排在
Step 5（表键限定化）之后，所以这条**落地即绿**。真正证明它有力的是 Step 13 的负控——
关掉 `QualifiedName` 那条缝之后重跑它必须红，见 Step 13 新增的第 4 步。反向也成立：
把这条 `add_test` 贴到 `d7ca710` 的树上跑，必红（表里是裸 `f`）。
**本阶段内这条门依赖的产物形状是不变的**（轮 6 复核：原稿这句写成了「Task 6 要把它重指」，
错了——本计划的 Task 6 是「带点 `import` 落地」，**不**动 `.nmod` 的粒度；每文件一份 `.nmod`
＋`.npack` 是**下一阶段（阶段 6）**的活）。也就是说 `proj_same_dir.nmod` 这个**合并**产物
在阶段 5 的 8 个提交里一直在，这条门不会在阶段内被搬走；到阶段 6 拆粒度时它必然连带改，
那是那份计划要负责的收口，这里只把预告写下，不在本计划里预留改动点。

**明确不做**（轮 6 定，写在这里防止顺手扩大战场）：
- 不给 `-func` 加「未匹配 ⇒ 报错／非零退出」的诊断。那是这个工具**今天就有**的毛病
  （实测 `ndisasm -func nosuch x.nmod` 打印完类表照常 `exit=0`），与包名无关，
  归阶段 6 的工具面一起看；在本阶段改它＝往一个正在全仓改键的提交里掺入无关行为。
- **更不许**为了让 `-func main` 继续能用而给 `:146` 加裸名回落或后缀匹配——那是本仓库
  明令禁止的兼容 shim，而且它会让「表键＝显示名」这条 D13 规则出现第二个真相源。
- 类表／结构体表（`:93`/`:114`）**不另加门**：它们的键面已经由本任务的
  `(5) TestBuiltinAndUserTypeNameCoexist` 在 `ModuleLoader::Load` 之后直接断言
  （同一批字符串，ndisasm 只是 `std::cout` 它们）；为一个 `cout` 再造 fixture 是空的。

- [ ] **Step 9: D10——流式字面量在编译期定身**

**轮 4 实测把这条从「改判据」升级成「换信息源」**：今天 `alib.n` 里的 `struct S`
配 `bs.readStruct("S")` 直接报 `ReadStruct type not found: S.`（ncc 实跑，`import alib;`
＋`alib.S r = bs.readStruct("S");`）。原因就在 `:277-282` 那段注释写明的实现——上溯的是
**调用者的 AST 作用域链**，而 `import` 从来不给链上添节点（D2：导入只开可见性）。
所以光换判据（裸名→限定名）不够，收集面必须换成注册表：

```cpp
	//Phase 5 D10: the literal resolves to a declaration NOW, from the
	//visible packages (own package + imported ones), and the string that
	//reaches the VM is rewritten to that declaration's table key. No
	//runtime bare-name fallback: the type tables are qualified-keyed.
	SnField* found = nullptr;
	size_t hits = 0;
	//收集面＝调用者所在模块的可见模块集合（同包＋已导入，复用 Task 3 的
	//PackageOf + IsModuleImported + ModuleRegistry 的成员遍历），
	//对每个可见模块扫 NK_Struct/NK_Class/NK_Enum 声明：
	//  字面量是裸名 -> 与该声明的**末段名**比；
	//  字面量带点   -> 与该声明的 **QualifiedName** 整串比。
	//两种形状都算命中，命中的 QualifiedName 相同则仍算唯一（同包重复会被
	//Task 6 的重名检查拦下，这里不重复实现）。
	if (hits > 1) {
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s: type name '%s' is ambiguous (%zu visible types share it); "
			"qualify it (e.g. 'pkg.%s').", pMethodDisp, typeName.c_str(),
			hits, typeName.c_str());
		m_pContext = pSavedContext;
		return nullptr;
	}
	if (found) {
		const std::string key = m_Env.Registry().QualifiedName(*found);
		lit.RewriteStringValue(key);   //见下一步
	}
```
**「带点字面量必须能用」是文案自己的承诺**（轮 4：原稿只收了裸名，`ambiguous`
诊断让用户写 `'pkg.S'`，而那条建议走到的是未命中分支——等于骗人）。
Step 1b 的 `(3d)` 就是钉这一条的正例。
`m_Env` 在这一步**只有只读用途**（`m_Env.Registry()`）；Step 6 往 `VmBackend` 注的是指针成员，
别把两边混成一个——resolver 这边手上是真引用，写法不变。

**歧义判据用 `QualifiedName` 比，不用 `OwnerOf != OwnerOf`**（轮 1 纠正）：`SnNamespace::
FindField`（`src/compiler/SnMisc.cpp:34-37`）是**首个名字匹配就返回**，所以今天那个上溯循环
根本到不了第二个同名类型——按 owner 不等来数 `hits` 会恒为 1，歧义诊断永不触发，`readStruct`
仍旧静默选中第一个。未命中仍走现有 `%s type not found: %s.` 文案
（`ExprResolverMemberBuiltins.cpp:314`）。
`SnLiteralExpr` 需要 `RewriteStringValue`（`include/nlang/compiler/SnExpressions.h`）：
只替换内部 `RnString` 数据，不改节点种类；注释写明「唯一用途＝把编译期已定身的类型名
变成表键」。

**未命中路径今天没有任何测试**（轮 3 实测：`tests/` 全仓 grep
`type not found`／`ReadStruct type not found` 零命中，原稿点的
`tests/e2e/stream_object_unknown_type` 这个文件不存在——`tests/e2e/` 只有
`bs_struct_class_*.n` 那一族**正例**）。所以这条文案没人在钉，Step 9 改判据时
必须自己补一条负例，否则改坏了也不知道——钉子已经在 Step 1b 的 `(3c)`
（`bs.readStruct("NoSuchType")` ⇒ 编译失败＋日志含 `type not found: NoSuchType`），
带点正例在 `(3d)`。VM 侧的
`IntrinsicsByteStream.cpp:211`／`IntrinsicsFileStream.cpp:223` 是运行期兜底文案，
编译期定身之后它们应当**不可达**，本任务不删它们（那是阶段 6 的清理面），但要在
提交说明里写明这条变化。

- [ ] **Step 10: 宿主 native 注册名改限定名，测试侧同步**

**注册表只有两处**（轮 6 实测订正，原稿的「三处＋三个工具 main」是猜的）：
`src/vm/TestNatives.h:61-64`（`natAdd`/`natConst`/`natFAdd`/`natPing` 四个裸名，
`RegisterTestNatives` 的函数体在 `:60-65`）＋ `tests/test_vm/test_native_host.cpp:76-85`
（`RegisterHs` 的 `hsMint`/`hsEcho`/`hsList`/`hsRaiseIo`/`hsRaiseBase`/`hsRawRandom`/
`hsSeededRandom`/`hsReadLine` 八个裸名，名字写在 `:77-84`）。
**三个工具的 main 一行都不用改**：`grep -rn "RegisterNative(" src/tools` ＝ 0 命中，
它们只调 `RegisterTestNatives(executor)`（`src/tools/ncc/main.cpp:131/381`、
`src/tools/ndb/main.cpp:56/159`、`src/tools/nvm/main.cpp:99`），表体在头文件里 ⇒ 改头文件、
重编工具即可。原稿列的 `ncc/main.cpp:35`、`ndb/main.cpp:42`、`nvm/main.cpp:36` 实测是
`PrintUsage` 的用法串与 include 区，跟注册面无关，**别去改**。

`TestNatives.h` 的新表＝**实测的 (package × 符号) 交叉积**，不是把四个名字各加一个前缀：
`native` 自由声明所在 TU 的 stem 就是包名，所以同一个 `natConst` 在四个包里各注册一次。
下面是 `grep -rn "^\s*native\b" tests/e2e --include='*.n'` 的全量结果（逐条对过，别凭印象加行）：

| fixture 源文件 | 声明 | 新键 |
|---|---|---|
| `native_basic.n:2` | `native int natConst()` | `native_basic.natConst` |
| `native_basic.n:3` | `native void natPing()` | `native_basic.natPing` |
| `native_args.n:2` | `native int natAdd(int,int)` | `native_args.natAdd` |
| `native_default.n:6` | `native int natAdd(int,int=5)` | `native_default.natAdd` |
| `native_float.n:2` | `native float natFAdd(float,float)` | `native_float.natFAdd` |
| `native_crossmod/nativelib.n:5` | `native int natConst()` | `nativelib.natConst` |
| `native_crossmod_default/nativelib.n:5` | `native int natAdd(int,int=5)` | `nativelib.natAdd` |
| `func_ref_native.n:3` | `native int natConst()` | `func_ref_native.natConst` |
| `native_method.n:7` | `public native int natConst()`（**方法**） | `natConst`（裸名，方法不带包名＝Step 4 的规矩） |
| `native_unregistered_throws.n:4` | `native int natGhost()` | 不注册（负例），但消息串里的名字变 `native_unregistered_throws.natGhost` |
| `native_body_reject.n:4`／`native_out_reject.n:4` | `natBad`／`natFill` | 不注册（`compile_error` 用例，产码之前就被拦） |
| `func_bound_native_reject.n:8/16`／`func_bound_iface_native_reject.n:12` | `natA`/`natV`/`run`（都是方法） | 不注册（`compile_error` 用例） |

⇒ `RegisterTestNatives` 里是 **8 条限定名 ＋ 1 条裸 `natConst`**（给 `native_method.n`）。
`natAdd`/`natFAdd`/`natPing` 的裸名**没有任何使用者**，按「不留兼容 shim」的规矩删掉。
注释同步：`:13-16` 那段「scripts calling natAdd/... get 'native function not registered'」
里的 `natAdd` 是旧拼写，改成表里真实存在的键。

`test_native_host.cpp` 侧：包名就是 `runSource` 的 `tag`（`:92` 写 `<tag>.n`、`:96` 只 push
这一份源 ⇒ TU stem＝包名）。`configure` 形参不带 tag，所以**给 `RegisterHs` 加一个包名参数**、
在调用点把已经写在那儿的 tag 字面量重复一次，比改 `std::function` 签名小：

```cpp
void RegisterHs(VmExecutor& e, const std::string& pkg) {
    //The package prefix is the TU the `native` declarations were compiled from
    //(runSource writes <tag>.n), because a free function's VM key is now
    //"<package>.<name>" (phase 5). Methods keep bare names - none here.
    e.RegisterNative(pkg + ".hsMint", &HsMint);
    ... // 八条同形
}
```
```cpp
    int rc = runSource("host_core", source,
                       [](VmExecutor& e) { RegisterHs(e, "host_core"); });
```
三处 `runSource` 调用点（`:157/:177/:191`）里只有 `:158` 与 `:178` 调 `RegisterHs`；
`host_write`（`:191`）那份只装 host IO、不注册 native，别顺手改它。

**生产侧 DLL 解析零改动**：stdlib 三段 `io`/`math`/`fs` 的包名本来就是单段，键仍是
`io.print` 这种形状，`VmExecutor::EnsureNativeAvailable`（`src/vm/VmExecutorNativeHost.cpp:141-156`）
「第一个点前＝DLL 名、余下＝符号名」的规则不动，`nlang_io.dll` 等三个生产 DLL 不用重编。
多段包（`vendor.graphics.hue`）按第一个点切会得到 `nlang_vendor.dll` 这个不存在的名字——
那是下面 D5 编译期诊断要拦的形状，不是运行期去补的洞。

**e2e manifest 侧同步**（原稿漏了这一整面）。轮 3 实测口径，别再写「14 个文件」：
`grep -n native tests/e2e/manifest.txt` ＝ **14 行**，其中 **11 行 `native_*`**
（`:736-747`：`native_basic`、`native_args`、`native_float`、`native_unregistered_throws`、
`native_body_reject`、`native_crossmod`、`native_crossmod_default`、`native_out_reject`、
`native_method`、`native_tostring_throws`、`native_default`）＋ **3 行 `func_*native*`**
（`:917 func_ref_native`、`:965 func_bound_native_reject`、`:966 func_bound_iface_native_reject`）。
这 14 行背后是 **16 个源文件**：`native_crossmod/` 与 `native_crossmod_default/` 各是目录，
内含 `nativelib.n`＋`nativemain.n`（＋`order.txt`）。逐条的键怎么写见上面的交叉积表
（轮 6 实测订正：原稿这句把 `native_basic.n` 说成 `native_basic.natAdd` —— 该文件里根本没有
`natAdd`，它声明的是 `natConst`／`natPing`；目录型用例的包名是 TU 的 stem `nativelib`，
**不是**目录名 `native_crossmod`）。推导命令留着备用：

```bash
grep -rln "native" tests/e2e --include='*.n' | while read f; do
  printf "%-46s pkg=%s\n" "$f" "$(basename "$f" .n)"; done
```

其中 4 行是 `compile_error` 用例（`native_body_reject`、`native_out_reject`、
`func_bound_native_reject`、`func_bound_iface_native_reject`）。**轮 6 实测金样本**：
`manifest.txt:740/744` 这两条只写 `compile_error` 不带期望串，`:965/:966` 两条的期望串是
`cannot reference the native method`（不含任何被注册符号名）⇒ **四条都不用改**。
`native_unregistered_throws 1`／`native_tostring_throws 1` 两行**只钉退出码**
（`:739`／`:746` 期望串就是 `1`，没有消息文本），所以金样本不动，但运行期消息确实变长——
`VmExecutorIntrinsics.cpp:26-27` 抛的是 `"NLang VM: native function not registered: " + callee.name`，
`callee.name` 就是表键（`native_unregistered_throws` 的自由声明 ⇒ `native_unregistered_throws.natGhost`；
`native_tostring_throws` 的是**方法** `toString` ⇒ 仍然裸名）。这条要在提交说明里点名，
别让后面的人以为漏了断言。

**不要**为了让 e2e 绿而给 `EnsureNativeAvailable`（`VmExecutorNativeHost.cpp:141-156`）
加裸名回落（那是兼容 shim，明确禁止）；改的是上面那两处注册表的内容，
三个工具的 main 不需要改（实测：它们只调 `RegisterTestNatives`，自己不含任何名字串）。


多段包内出现 `native` → 编译期诊断（D5）：**加在 `FillNativeFunctionRecord` 的调用侧**
（`VmBackend.cpp:133-136`，此处 `func` 在手、`PackageOf` 可查），条件 `PackageOf(func)` 含 `.`；
文案指名所在包，说明 DLL 选择按第一个点切、阶段 6 才解限。不要在 resolver 里加：那儿拿不到
产码侧的包判定，会变成第二条规则源。

**不做**「把 native 注册表抽成独立 `NativeRegistry`」的重构（轮 1 有审阅者建议此路，被否）：
本阶段已经是全仓最宽的键面改动，再叠一层 API 迁移会把可回滚的提交变成跨工具连锁；改三个
工具 main 的注册串是更小的做法。等阶段 6 的链接面真需要按包持有 native 表时再谈。

- [ ] **Step 11: 全量＋e2e 语料＋文档门**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
D:/dev/miniconda3/python.exe tests/e2e/run_e2e_tests.py \
  build-dev/src/tools/ncc/Release/ncc.exe build-dev/src/tools/nvm/Release/nvm.exe \
  | tee /tmp/e2e.log; grep -c '^SKIP' /tmp/e2e.log      # 期望：0
```
ctest 期望 **64/64**（63 条基线＋Step 8c 新增的 `ndisasm_qualified_func_name`——本计划全篇
唯一一条新增 ctest 条目，此后的门限就是 64）。特别核对：`test_stdlib`、`test_native_*`、
`check_nmod_determinism.py`（序列化布局变了，仍须字节确定）。
IDE 面（`mainwindow_tests`、`nide_deploy_check`）必须串行跑到绿。

> **轮 4 实测纠正：`tests/e2e/manifest.txt` 不在 ctest 里**。`grep -n manifest tests/CMakeLists.txt`
> ＝ 0 命中，ctest 侧只有 `e2e_compile`（`tests/CMakeLists.txt:914`，只编 `examples/hello.n`）
> 和若干 `proj_*` 直接 `add_test`；`manifest.txt` 的 1000＋行语料只有
> `tests/e2e/run_e2e_tests.py` 会跑。所以原稿「特别核对 `e2e_*`（真实 `.n` 语料）」这句
> 是空承诺——金样本（`:1021/1022/1025/1026/1029`）与 Task 6 翻转的 `stdlib_reserved_names`
> **不在陈述的门里执行**。上面第三条命令就是补的这一环，runner 默认路径指向不存在的
> `build/`（`run_e2e_tests.py:26-29`），必须显式传 `build-dev` 的二进制，而且只能是
> `src/tools/…` 那一份——用 `build-dev/tests/Release/` 会让 13 条 `dbg_*`／`dbgm_*` 全红
> （ndb 路径推不出来，见 Task 1 Step 2 的「runner 的 argv 形状」块），而本步改的金样本
> **正是**那 13 条里的五条。门期望 **977 passed / 6 failed**（基线 974＋Task 1 的三条 `qhead_*`；
> 六条既有失败逐字见那个块）。
> 凡改动 `tests/e2e/**` 的任务（Task 1、Task 4、Task 5、Task 6）的门里都要带这条命令。

**「e2e 输出逐字不变」在本任务不成立**（轮 1 纠正）：D13 之后崩溃回溯／停点显示的是限定名。
**轮 3 实测的金样本清单**（`tests/e2e/manifest.txt`，只有这 5 行的期望串里嵌了函数名）：

| 行 | 现状期望串 | 限定化之后 |
| --- | --- | --- |
| `:1021` | `Breakpoint 1, main (dbg_break_continue.n:9)` | `Breakpoint 1, dbg_break_continue.main (...)`（**轮 5 纠正**：单文件用例，包名＝stem＝`dbg_break_continue`，不是 `main`——原稿把它抄成了 `main.main`，与同表 `:1026/:1029` 的派生规则自相矛盾） |
| `:1022` | `Stopped: inner (dbg_step_semantics.n:3)` | `Stopped: dbg_step_semantics.inner (...)` |
| `:1025` | `Breakpoint 1, triple (mathutil.n:3)` | `Breakpoint 1, mathutil.triple (...)`（多文件项目里 `mathutil.n` 在项目根 ⇒ 包名 `mathutil`） |
| `:1026` | `Breakpoint 1, main (dbg_quit_eof.n:4)` | 用例文件是 `dbg_quit_eof.n` ⇒ `dbg_quit_eof.main` |
| `:1029` | `Throw: main (dbg_catch_throw.n:4)` | `Throw: dbg_catch_throw.main (...)` |

`:1023/:1024/:1027/:1028/:1030/:1031/:1032` 的期望串不含函数名，不动。
**先确认每一条的包名派生**（`DeriveModulePath` 相对项目根；`:1022/:1026/:1029` 是单文件场景，
包名＝stem＝去掉 `.n` 的文件名，不是 `main`——只有 `main.n` 那个文件的包名才是 `main`），
再把新串**逐字**写进 manifest，不要凭记忆拼。跑全量前逐条看过 diff，确认每一处都只是
「名字变长」而不是行为变化，再放行。
`native_unregistered_throws.n`／`native_tostring_throws.n` 这类**诊断文案**用例也要重看：
它们的期望串吃的是运行期异常消息，若消息里带函数名同样会变长；不带就原样通过。

- [ ] **Step 12: Commit（先提交，再做负控）**

```bash
git add src/vm src/compiler tests/packaging/verify_package.py include/nlang/vm/CompiledModule.h \
        include/nlang/compiler/SnExpressions.h tests/test_vm/test_library_source.cpp \
        tests/test_vm/test_debugger.cpp tests/test_vm/test_native_host.cpp \
        tests/test_nide/test_debug_client.cpp \
        tests/test_nide/test_mainwindow.cpp tests/e2e/manifest.txt tests/CMakeLists.txt src/tools \
        docs/user_manual/en/vm-architecture/module-serialization.md docs/user_manual/zh/vm-architecture/module-serialization.md
git commit -m "feat(vm): qualify type and function table keys by package path"
```

> 轮 4 实测纠正：原稿的 add 清单**漏了三个测试文件**（`test_debugger.cpp` 的 14 处表键串＋
> 三处 `0x0C` 版本钉、`test_debug_client.cpp:162/178`、`test_mainwindow.cpp:2284/2556`）。
> 漏掉的后果是确定的：Step 13 的 `git status --porcelain` 会在基线行之外多出条目，而且这个提交单独检出后
> `debugger_tests` 与 nide 用例是红的（改动没进快照）。`src/tools` 覆盖 ndb／ndisasm 的
> 展示与过滤拼写。
>
> **轮 6 再补两条（同一扇门，同一后果）**：
> - `tests/test_vm/test_native_host.cpp` —— Step 10 的宿主注册表改的就是它（`RegisterHs`
>   的 (package × symbol) 交叉），不 add 则本提交单独检出时 `native_host_tests` 用的是旧
>   注册表配新键 ⇒ 红，且 Step 13 的 `git status --porcelain` 非空。
> - 两篇 `module-serialization.md` —— Step 8 第 5 条 Owns 六处版本号文档（en `:15/:23/:34`、
>   zh `:13/:21/:28`），Task 8 的文档门不含这三处 `12`（它 grep 的是 `namespace` 一词），
>   所以留在树里没人收尾。


- [ ] **Step 13: 负控——关掉缝即失败（必须在 Step 12 之后跑，轮 2/3 两次独立指出）**

原稿把负控排在提交**之前**，而它收尾用的是 `git checkout -- src/vm/backend/Register.cpp`：
那会连带抹掉本任务在同一个文件里尚未提交的 Step 4/5/6 改动。所以顺序是提交→负控→校验干净：

```bash
# 1) 临时把缝关掉（只改这一处，不提交）
#    Register.cpp 的 QualifiedName → return field.Name();
cmake --build build-dev --config Release -j 8 --target ncc test_library_source
#    （轮 7 补 `test_library_source`，C3：`Register.cpp` 编进 `nlang_vm` 静态库，
#     `tests/test_vm/test_library_source.cpp` 的用例是**进程内** compileRun——
#     只重建 `ncc` 的话测试二进制里那份还是缝开着的老代码，
#     Step 1 的场景会「照旧通过」＝假绿，负控什么都没证明。
#     Step 1 若同时落了 `test_debugger.cpp` 的断言，target 列表再加 `test_debugger`。）
# 2) 跑 Step 1 的场景，必须复现「串布局」症状（跨包同名类型互相覆盖／入口找不到）
ctest --test-dir build-dev/tests -C Release -N -R "library_source_tests"   # 期望：Total Tests: 1
ctest --test-dir build-dev/tests -C Release -R "library_source_tests"
#    期望：Failed，末尾 `1 tests failed out of 1`；进程内用例的 `=== Results: %d passed, %d failed ===`
#    里 failed ≥ 1（`test_library_source.cpp:451` 实测 `return g_fail > 0 ? 1 : 0;`，
#    失败一定带非 0 退出码，ctest 才认得）。
#    （条目名实测：`tests/CMakeLists.txt:150` 的 `add_test(NAME library_source_tests …)`——
#     跟可执行目标 `test_library_source` 不同名，写 `-R test_library_source` 会命中 0 条、
#     打印 `No tests were found!!!` 且**退出 0**，见上面 3a 的同一条防呆。）
# 3) 同一把关缝的 ncc 上跑 Step 8c 那条新门，必须红（轮 6 补：这条门唯一的失败侧证据）
#    3a) 先数选中条数，再跑（轮 7 重写，C1）。`-R` 对不上的名字**不报错**：
#        实测 `ctest -R <不存在的名字>` 只打印 `No tests were found!!!` 并以 **0** 退出，
#        所以「跑完没红」完全可能是「根本没跑到」。条数检查是这里唯一的防呆：
ctest --test-dir build-dev/tests -C Release -N \
  -R "proj_same_dir_compile|ndisasm_qualified_func_name"
#    期望：末尾 `Total Tests: 2`（两条都叫得出名字）。
#    只有 1 条 ⇒ Step 8c 的新条目没注册（名字拼写／add_test 没进这轮的 CMakeLists）。
#    0 条 ⇒ 两条名字都错，整步作废重跑。
# 3b) 再跑同一条 pattern：
ctest --test-dir build-dev/tests -C Release \
  -R "proj_same_dir_compile|ndisasm_qualified_func_name"
#    期望（**out of 2 这个数字是承重的**，只写「有失败」拦不住空跑）：
#      proj_same_dir_compile 仍 Passed（关缝只是回到旧键形状，编译不失败），
#      ndisasm_qualified_func_name **Failed**，
#      末尾 `100% tests passed, 1 tests failed out of 2` ＋ `The following test FAILED: ndisasm_qualified_func_name`。
#    （轮 7 删掉原稿的「若显示 Not Run ⇒ DEPENDS 没生效」那句：两条都在 `-R` 选中集里，
#     DEPENDS 在集内一定生效，`Not Run` 这个状态在本命令下**不可达**；
#     新条目没注册的情况由 3a 的 `Total Tests: 2` 兜住，不需要那句。）
# 4) 恢复并核对工作树
git checkout HEAD -- src/vm/backend/Register.cpp
cmake --build build-dev --config Release -j 8 --target ncc test_library_source
git status --porcelain          # 期望：只剩 `?? main.n` 这一行（Global Constraints 的基线，工作树里恒有）
ctest --test-dir build-dev/tests -C Release -R "library_source_tests"   # 期望：Passed（缝已恢复）
grep -c "KeyOf" src/vm/backend/Register.cpp   # 期望：与提交里一致（改动没被抹掉）
```
（`verification-before-completion`：这条证明 Step 1 的测试非空。`git checkout HEAD --` 而不是
`git checkout --`：提交之后两者等价，但带 `HEAD` 的写法在「忘了先提交」时会立刻暴露。
最后那条 `ctest -R library_source_tests` 是恢复侧的闭环——光看 `git status` 干净不能证明
重链接过的测试二进制又变绿了。）

---

### Task 5: 删除 `namespace` 关键字 ＋ 容器判定退役 ＋ 索引面（一次闭合）

> **轮 1 把三件事并到了这一步**（原稿分在 Task 3／Task 5／Task 7，分开必红）：
> ① 删产生式；② 去 stdlib／fixture 的外壳；③ 退役 `ModuleRegistry` 的两处容器判定
> ＋改写 `langservice::SymbolIndex` 的名字来源。
> 理由是前置事实那条：`MergeFrom` 搬进 root 的是**壳节点本身**，所以「成员落到 root」
> 与「不再下钻找壳」必须是同一个提交；而 `SymbolIndex.cpp:98-99` 的正则只认
> `^\s*namespace\s+(\w+)`，去壳之后索引直接空——`test_library_index.cpp:47` 的
> `size() == 38` 与 `BuildEnvironment.cpp:33-34`（构造时 `LoadLibraryDir`）会当场红。
> 这四处任何一处单独提交都过不了门。

**Files:**
- Modify: `src/compiler/grammar/nlang.y:359`（`%token KT_Namespace`）、`:191`（union `v_pNamespace`）、`:270-272`（`%type`）、`:534-551`（`NamespaceMember: Namespace`）、`:553-555`（`Namespace` 产生式）、`:1229-1245`（账本）
- Modify: `src/compiler/grammar/nlang.l:174`
- Modify: `src/compiler/builder/ModuleRegistry.cpp:280-311`（`CompiledInFunctions` 去 `isLibrary` 分支）、`:359-397`（`FindModuleType` 去 `pContainer` 选取）
- Modify: `src/langservice/SymbolIndex.cpp`（`kHead` 正则 `:98-99`＋`ConsumeIndexLine:154-155`
  的硬门：文件级名字改为**路径派生**，`SymbolIndex::LoadLibraryDir` 在 **`:234-246`**
  ——轮 3 勘误：`:14-34` 是匿名 namespace 里的 `Trim`/`ParseParams` 小工具，不是
  `LoadLibraryDir`；调用点是 `BuildEnvironment.cpp:34` 与 `MainWindow.cpp:369/381`）
- Modify: `stdlib/io.n:5+`、`stdlib/math.n:5+`、`stdlib/fs.n:5+`（去外壳＋减缩进）
- Modify: `tests/fixtures/native/mylib/mylib.n:13-32`、`tests/fixtures/native/mixlib/mixlib.n:9-20`
  （轮 3 勘误：两个文件各在自己同名子目录下，不在 `tests/fixtures/native/` 根下）
- Modify: 内嵌 fixture 源码里的 `namespace`：`test_module_import.cpp`（**轮 6 复数订正：9 行，
  原稿列 8 行漏了 `:941`／`:948` 这两条嵌套内层 `namespace B {`** ⇒ 正确清单是
  810/863/869/940/941/947/948/1378/1381；而 `:471` **不在这个面上**，它是
  `"namespace."` 这个**期望串片段**的一部分（`:468-472` 整条测的是保留名诊断，归 Task 6 Step 5
  第 1 条），把它当 fixture 源去壳会改错文件位置。**`:853-873` 是「一个 namespace 由多个 TU
  合并而成」的形状，删关键词后这个形状不存在，必须整条重写或删除**、
  `:1363-1386` 的「跨目录共享 namespace ⇒ 裸名不可达」同理，它测的正是本阶段要消灭的那条限制）、
  `test_thirdparty.cpp:66`、`test_symbol_index.cpp:99/125`（**轮 4 补：这条在
  `tests/test_langservice/` 下，而下面的收口 grep 原本没扫这个目录**；`:93-116` 的
  `TestNlangFunctionIsNotNative` 用 `namespace mylib {` 内嵌源 ＋ `Resolve("mylib","add")`，
  去壳后 `mylib.n` 直接躺库根 ⇒ 包名仍是 stem `mylib`，断言不变；`:118-139` 的
  **`TestAllmanNamespaceBraces` 整条前提消失**——它钉的就是「`namespace` 的 `{` 换行也能认」，
  `kHead` 一删就没有可认的头，必须**改写成钉新事实**而不是删掉了事，见 Step 3c）、`test_library_source.cpp`（105/141/150/161/163/177/206/261/307/349）、
  `test_mainwindow.cpp:611`、`test_searchpath_integration.cpp:79`、**Task 4 Step 1 新加的 `rb3_named` 场景**（本任务里它必须跟着去壳，否则下一步就红）、
  **Task 2 Step 1 的 `TestQualifiedBaseAndCast`**（轮 2 补：它的 `alib.n` 内嵌源写的是
  `"namespace alib {\nclass B {..."}`，Task 4/5 之后不跟着去壳就当场红）、
  **Task 2 Step 7 的三段链负例**（同一份 fixture 写法）；
  **轮 5 补的两处漏项**：
  ① **Task 1 的 `tests/e2e/qhead_call/alib.n`＋`qhead_deep/alib.n`****（本节末尾「收口办法」那条
  grep 会扫到 `tests/e2e`，但清单里原本没这两条，且它们的**后果不是「不美观」而是门红**：
  Task 5 之后 `namespace alib {` 写不出来 ⇒ ncc 对这两条用例报 `syntax error` ⇒
  `manifest.txt` 的 `compile_error Malformed qualified type reference` 三行金样本必然红。
  处置：两份 `alib.n` 去外壳＋减缩进，`qhead_*` 的判据（编译失败＋那句文案）不变，因为
  `alib.twice(1)` 这条链在无语法的库源上照样是非法类型头）；
  ② **Task 4 Step 1b 的 `(1)` `TestPathBeatsShellName`**（`tests/test_vm/test_library_source.cpp`）
  ——整条**前提消失**：它写的是「文件叫 `zlib.n`、壳里写 `namespace alib`」，删语法后这个源
  根本表达不出来。处置**不是删掉了事**（D8 的钉子会跟着没）：`(1)` 的正例改成裸体库源
  `int twice(int x) { return x * 2; }`＋`import zlib;`＋`zlib.twice(3)`，断言不变、函数名保留
  但在注释里改写事实（「包名只有路径这一个来源，壳名这一概念已经不存在，所以「压过」不再
  是可测关系」）；
  **轮 6 实测订正：`(1b)` 的库源也必须一起去壳**（原稿说「`(1b)` 的源文本不动」是错的）。
  `(1b)` 的第二份 `zlib.n` 同样写着 `namespace alib {`（Task 4 Step 1b `(1b)` 的代码块），
  Task 5 之后那一份**解析就死**：`Build()` 报的是 `syntax error`，`main.n` 里的 `alib.twice(3)`
  根本走不到 gate ⇒ `CHECK(compileLog(dir2).find("not imported") …)` 红，而它前面那条
  `!compileDir(dir2)` 会**照样绿**（编译确实失败），于是这条用例变成「绿一半红一半」的
  假线索。正确形状：两份 `zlib.n` 都写裸体 `int twice(int x) { return x * 2; }`，
  `main.n` 里保留 `import zlib;` ＋ `alib.twice(3)`（钉的是「`alib` 这个包没被导入」，
  与壳无关），断言两句都不动。

  ③ **Task 3 Step 1 的 `(c)`**（`tests/test_compiler/test_module_import.cpp`：`findMember(… ,
  NK_Namespace, "io")` ＋ `static_cast<const SnNamespace*>(pIo)->Members()` 那次下钻）——它钉的是
  **今天的树形状**，去壳后 root 上没有 `io` 壳节点，`QVERIFY2(pIo != nullptr, …)` 直接红。
  **本任务里就地改写**成新形状上的同一条事实：`findMember(ioRoot.Members(), NK_Function, "print")`，
  下面 `PackageOf == "io"`／`QualifiedName == "io.print"` 两句**一字不改**——改写后的 `(c)`
  恰好把 D12（去壳前后键不变）钉在同一处。**不要**写成「先扫 root、扫不到再下钻壳」的双形状
  helper：那是本仓库明令禁止的兼容 shim，而且它会让 `(c)` 在两种树上都「可能」绿。
  **收口办法**（**轮 6 重写**：原稿那条 `grep -rn 'namespace' … | grep -v '^\s*//'` 有两个毛病——
  ① 后面的 `grep -v '^\s*//'` 是**空转过滤器**（`-rn` 输出行首永远是 `路径:行号:`，不可能匹配
  `^\s*//`，实测 7 进 7 出，一条注释都没去掉）；② 范围既漏面（没有 `examples/`、
  `tests/fixtures/`）又混面（把 C++ 的 `SnNamespace`／`NK_Namespace` 类型名和测试标签一起算进来）。
  轮 6 试过的「一把梭」写法 `grep -rni namespace stdlib tests tools src include examples`
  ＝ **1297 命中**（轮 7 按这条写死的七条目录重测。轮 6 在此记的是 715，**换任何目录子集都复现不出来**，
  所以按可复现的 1297 为准）。加 `-I`（跳过 `tools/` 下 6 个二进制）＝ **1291**，逐目录
  **src 849／tests 233／include 193／tools 6／stdlib 6／examples 4**。这份清单没人能勾，所以它不能当门。
  改成**两条各答一个问题**）：

  ```bash
  #(1) 磁盘上的 NLang 源里还有没有 `namespace` 声明／自述？
  grep -rn "namespace [A-Za-z_]" --include='*.n' stdlib tests examples
  #(2) C++ 测试码里内嵌的 NLang 源字符串还有没有？
  grep -rnE '" *namespace [A-Za-z_]' tests --include='*.cpp'
  ```
  基线（`d7ca710` 实测）：**(1) ＝ 10 命中／9 文件** —— 3 条要删的外壳
  （`stdlib/{io,math,fs}.n:5`）＋ 2 条磁盘 fixture 外壳（`mixlib.n:9`、`mylib.n:13`）
  ＋ 5 条注释（`import_io_missing.n:2`、`mixlib.n:4`、`examples/stdlib_{io,fs,math}.n:1`）。
  **(2) ＝ 24 命中／6 文件**，逐文件：`test_library_source.cpp` **10**（105/141/150/161/163/
  177/206/261/307/349）、`test_module_import.cpp` **9**（810/863/869/940/941/947/948/1378/1381，
  全是字面量行）、`test_symbol_index.cpp` **2**（99/125）、`test_mainwindow.cpp` **1**（611，
  断言文案 `contains("namespace io")`，跟着 Step 6b 的文案面走）、
  `test_searchpath_integration.cpp` **1**（79）、`test_thirdparty.cpp` **1**（66）。
  **这两条 pattern 都是轮 7 换过的**，换的理由要记下来，否则下次照旧命令跑必漏：
  ① 原 (2) 写作 `'"namespace \|<< "namespace'`，实测只有 **22 命中**——它漏的是
  **缩进的嵌套外壳**（`test_module_import.cpp:941/:948` 的 `"    namespace B {\n"`，
  引号后有空格），而这两行恰是本任务要删的声明；新写法 `" *namespace [A-Za-z_]` 一并进行数
  前缀，24 命中把它兜进来。② 原 (1) 记的「8 文件」漏算一个（10 命中散在 9 个文件里，
  `mixlib.n` 同时贡献声明行与注释行）。
  把命中逐条对着本清单勾，**以 grep 结果为准**，行号只是提示——前四个任务各自会新增内嵌源。
  **两个坑**（轮 5＋轮 6 实测，轮 7 纠正坑①的归属）：① `tests/e2e/stdlib_reserved_names.n:1/5/6/8`
  这 4 行**上面两条 pattern 一条都不命中**（轮 7 实测：`grep -n "namespace [A-Za-z_]"
  tests/e2e/stdlib_reserved_names.n` ＝ **0 命中**——`namespace` 在这几行要么是行尾裸词
  （`// function named after a namespace`），要么跟着 `s`（`namespaces (math/io/fs)`），
  而 `[A-Za-z_]` 要求紧跟一个字母）。会命中它们的是**宽 grep**
  （`grep -rn namespace tests/e2e --include='*.n'`，Task 5 Step 7 的 e2e 说明里用的就是这条，
  实测 7 命中／4 文件）。后果有两个方向，都要记住：拿宽 grep 当收口证据＝看见 4 行以为要改，
  实际这个文件本任务不动；拿 (1)/(2) 当收口证据＝它真的不在结果里，别把「没命中」读成
  「已经改完了」。它的归属是 **Task 6 Step 5 第 3 条**（保留名表退役时把 `compile_error`
  翻转成运行用例），文件是「用 `io`/`fs`/`math` 当用户名」的负例语料，钉的不是声明，
  删外壳会把用例改废；
  ② 别在命令后面再加 `grep -v 'namespace fs'` 之类的顺口过滤，`stdlib/fs.n:5` 的
  `namespace fs {` 正是本任务要删的那一行。

  **不改的三面**（轮 6 定，防止扫的人顺手扩大战场）：
  `SnNamespace`／`NK_Namespace`（AST 容器类型与节点种类——包是**注册表里由路径派生的条目**，
  不是 AST 节点，unit root 仍然是这个类）；C++ 自己的 `namespace nlang {`／
  `namespace {`／`namespace fs = std::filesystem;`；`SymbolIndex.cpp:99` 那条 `kHead`
  **正则**（它的删除归 Step 3c）。


- Modify: `tests/test_compiler/test_library_index.cpp:47`（38 这个数按新的名字来源重算并写进注释）
- Modify: `tools/nlang-docs/src/nlang_docs/highlight.py:24-26`（注释里的三个计数
  **40 keywords／54 reserved words** 跟着降成 **39／53**，轮 7 实测原文在 `:24`
  「40 keywords + 11 builtin-type words … = the scanner's 54 reserved words」）
  ＋ `:32`（`NLANG_KEYWORDS` 里的 `namespace`）
- **不改**：`tools/nlang-docs/tests/test_highlight.py:77-90`（轮 7 实测：`:89` 断言的是
  `scanner == (NLANG_KEYWORDS | NLANG_TYPES | NLANG_CONSTANTS) - docs_types`，
  而 `scanner` 集合是 `:81-83` 从 `nlang.l` 现拉的——Step 2 删扫描器关键字、Step 6 删
  lexer 集合，两边**同一步一起缩**，这条只会红在「一边动了另一边没动」的场合，
  本任务正常做完它保持绿。所以它**不是本任务的负向证据**：`namespace` 真被删掉的证据是
  Step 1 的解析失败测试，别拿「test_highlight 没红」当通过理由。）
- Modify: Step 6b 的文案面（轮 5 新增，逐条给在 Step 6b 的表里）：
  `src/compiler/builder/ExprResolverStdLib.cpp:87-88`、`src/compiler/builder/ExprResolverTypes.cpp:355/:373`（轮 6 实测：原稿写 `:371`，那是 `if` 首行）、
  `src/compiler/SnMisc.cpp:78/:191`、`src/compiler/builder/ModuleRegistryGate.cpp:83/:117`、
  `src/vm/NativeLibraryLoader.cpp:125` ＋ 三处跟着改的断言／金样本
  （`tests/test_compiler/test_module_import.cpp:1427-1460`、`tests/e2e/manifest.txt:1010`）
- **不改**：`tools/source_size_guard/check_source_size.py:38` 的 `KEYWORDS`（轮 7 驳回原稿的
  「已核实，删掉」——那是**类别错误**。实测该元组只在 `:79` 用一次，用法是
  `line.lstrip().startswith(tuple(k + " " for k in KEYWORDS))`，作用是**把 C++ 自己的
  `namespace nlang {`／`using namespace …` 这类行从「函数定义」判定里排除**，跟 NLang 的
  `namespace` 关键字没有半点关系；删了它只会让守卫脚本对 C++ 源少一层防误报。
  而且今天删不删都**无可观测差别**：`SIG_RE`（`:42-46`）要求行里有 `(`，
  `namespace X {` 根本进不到这一分支，守卫当前 `exit 0`。）
- Test: 上述文件自身

- [ ] **Step 1: 先加负例测试**

`tests/test_compiler/test_import_parse.cpp`（或 `test_scanner.cpp`，取已有关键字测试的
那个文件）加一条：源码含 `namespace x {}` 必须解析失败并给出
`syntax error` 而不是内部断言。

**正例不用新写**（轮 2 纠正原稿的预测）：`tests/test_vm/test_library_source.cpp:402`
的 `TestModuleTypeWithoutNamespaceWrapper` 已经在用**无外壳**的库源
（`kRootModuleSource:382-390`，顶层 `struct Box` ＋ `int total(Box)`）并断言
`other.Box`／`other.total(b)` 解析成功（`rc == 0`），它在 `d7ca710` 的 63 条基线里就是绿的——
`FindModuleType` 的 root 回落（`ModuleRegistry.cpp:377-385` 的 `pContainer == nullptr`
分支）今天已服务这种形状。所以本任务**没有**「库文件不写外壳就不行」的失败可等，
TDD 的失败侧只有负例一条。原稿写「正例必失败」是错的，照它做会在 Step 2 白跑一轮。

- [ ] **Step 1b: 设计 §3 的行为钉子（轮 4 补：这条正例原本没排进任何任务）**

设计 §3「项目模块也是限定身份」是 §0.2 的正面对手戏，Task 3 Step 2 原稿把它指到
「Task 5 Step 4」——那一步是 bison 冲突计数，不吃编译＋执行，等于没排。现落在本步：
它是**加进来就绿**的回归钉，钉的是本任务（去壳）最容易撞碎的那面墙。
`test_module_import.cpp:1020` 今天已经证明 `utils.helper.help()` 这条路是通的，
所以这里补的是**同末段两目录**那一半：`utils/helper.n` 与 `core/helper.n`。

脚手架要加一个字段（现有 `GateProjectOptions:192-201` 只能写 `proj/utils/helper.n` 一份
helper，`withUtils2MyClass` 写的是 `utils2/MyClass.n`、末段不同名，凑不出这个形状）：

```cpp
    bool withCoreHelper = false;  //also register core/helper.n (same stem, other dir)
```
并在 `buildGateProject` 的写入串里跟着 `withUtils2MyClass` 的写法加一条
`writeFile(proj / "core" / "helper.n", "int help() { return 5; }\nstruct Cfg { int c; }\n")`
＋ `create_directories(proj / "core", fsError)` ＋ `m_SourceFiles.push_back(...)`
（`:308-313` 那一段就是注册点的形状）。用例：

```cpp
    //Phase 5 §3: two project units sharing a LAST path segment (utils/helper.n
    //and core/helper.n) are two packages, and each one's `help`/`Cfg` keeps
    //its own identity. Shell removal (this task) is what could merge them:
    //both members land on root with equal_range hits, and the only thing
    //telling them apart is the owner tag.
    void sameStemProjectUnitsStayIndependent()
    {
        GateProjectOptions opts;
        opts.withCoreHelper = true;
        opts.szMainBody =
            "import utils.helper;\n"
            "import core.helper;\n"
            "int main() { return utils.helper.help() + core.helper.help(); }\n";
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "two same-stem project units must build together");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(moduleIndexOfPath(reg, "utils.helper") != ModuleRegistry::NO_OWNER);
        QVERIFY(moduleIndexOfPath(reg, "core.helper") != ModuleRegistry::NO_OWNER);
        QVERIFY(moduleIndexOfPath(reg, "utils.helper")
                != moduleIndexOfPath(reg, "core.helper"));
        //Both `help` members are on root now, in the SAME name-dict bucket.
        //A single findMember() hit would be a coin flip -- collect the whole
        //equal_range and check the owner SET (same traversal shape as
        //CompiledInFunctions, ModuleRegistry.cpp:280-311).
        auto range = res.builder->TreeRootView().Members()
                         .NameDict().equal_range("help");
        std::set<std::string> owners;
        for (auto iField = range.first; iField != range.second; ++iField)
            if (iField->second->Kind() == NK_Function)
                owners.insert(reg.ModulePathOf(reg.OwnerOf(*iField->second)));
        QCOMPARE(owners.size(), static_cast<size_t>(2));
        QVERIFY(owners.count("utils.helper") == 1);
        QVERIFY(owners.count("core.helper") == 1);
        //Every hit's key starts with its own package -- no bare `help` left.
        for (auto iField = range.first; iField != range.second; ++iField)
            if (iField->second->Kind() == NK_Function)
                QVERIFY(reg.QualifiedName(*iField->second)
                            .rfind("help", 0) != 0);
    }
```
`<set>` 若该文件没包含要补。`ModulePathOf(:99)`／`OwnerOf(:173)`／`QualifiedName` 三个名字以现场头文件为准
（`src/compiler/builder/ModuleRegistry.h`，**不在 `include/nlang/compiler/` 下**；
`QualifiedName` 是 Task 3 Step 3 刚定义的），跑编译门前
`grep -n 'ModulePathOf\|std::string QualifiedName' src/compiler/builder/ModuleRegistry.h`
对一遍。**执行面**（`return 8`）由本文件既有的
跑通型用例覆盖，若现场没有可复用的 run helper，就在提交说明里写明只钉到了编译＋解析层，
别为了凑数新造一个 executor 入口。

- [ ] **Step 2: 跑它确认失败**（负例：`namespace` 今天仍被接受）。

- [ ] **Step 3: 删产生式与词法**

`nlang.y`：删 `Namespace:` 产生式（`:553-555`）、`NamespaceMember` 的 `Namespace` 分支
（`:534-536`，删完 `Function` 成为第一个 alternative）、`%token KT_Namespace`、union 的
`v_pNamespace`、`%type <v_pNamespace>`。
`nlang.l:174` 删 `"namespace" { return KT_Namespace; }`（此后 `namespace` 是普通标识符）。
`NamespaceMemberList` 保留（root 成员表），注释改成「文件顶层成员表；容器＝文件自身的包」。

- [ ] **Step 3b: 两处容器判定退役（从 Task 3 搬来）**

去壳之后库成员才真正在 root 上，这时才能删下钻。`CompiledInFunctions`
（`ModuleRegistry.cpp:280-311`）的函数体变成：

```cpp
	std::vector<SnFunction*> owned;
	SnNamespace* pRoot = TheAST().Root();
	if (pRoot == nullptr)
		return owned;
	//Library units and project modules are the same kind of unit now:
	//every member sits on root and carries its owner tag, so the
	//`namespace <path>` container lookup is gone. Identity = PackageOf.
	auto range = pRoot->Members().NameDict().equal_range(calleeName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField& member = *iField->second;
		if (member.Kind() == NK_Function && OwnerOf(member) == moduleIndex)
			owned.push_back(static_cast<SnFunction*>(&member));
	}
	return owned;
```
`FindModuleType`（`:359-397`）同样去掉 `pContainer` 选取，只留 root ＋ `equal_range` ＋
`IsBindableTypeDecl && OwnerOf(*iField->second) == i`。同步 `ModuleFunctions`（`:313-338`）
的调用点与 `ModuleRegistry.h` 的私有签名（去掉 `path` 参数）。

**这两处与去壳同提交的证明**：先提交 Step 3/5 而去钻还留着 → 下钻找不到壳、库符号全丢；
先提交 Step 3b 而壳还在 → root 上扫不到壳内成员，同样全丢。两种拆法都红，所以不能拆。

- [ ] **Step 3c: `SymbolIndex` 的名字来源改路径（从 Task 7 搬来）**

`src/langservice/SymbolIndex.cpp:98-99` 的 `kHead` 只认 `namespace <name>`；去壳后一行都不匹配，
`ConsumeIndexLine:154-155` 的硬门会让整个索引为空 → `BuildEnvironment.cpp:33-34` 载入 stdlib
目录后 `HasNamespace("io")`（`:57`）为假，codegen 的库签名与 `test_library_index.cpp` 一起红。
改法：`LoadLibraryDir` 遍历时把**文件相对库根的点分路径**直接作为 `sym.ns`（和编译器
`DeriveModulePath` 同一条规则，不引入第二种拼写），删掉 `kHead` 对 `namespace` 的依赖；
`test_symbol_index.cpp:99/125` 的用例源同步去壳。
`test_library_index.cpp:47` 的 `size() == 38`（注释「io 5, math 25, fs 8」）**预期不变**
（轮 3 实测：`stdlib/{io,math,fs}.n` 的声明条数就是 5／25／8＝38，索引的是**声明行**，
去壳只改 `sym.ns` 的来源、不改声明的条数；`Resolve("io", "print")`（`:50`）在新规则下
仍然成立，因为 `io.n` 按路径派生得到的包名恰好还是 `io`＝D12 的锚点）。
所以这一步的正确动作是：**把注释里「namespaces」的措辞改成「package paths（由文件路径派生）」，
数字保持 38**，并加一条断言钉住来源变了这件事——现有
`CHECK(env.LibraryIndex().Resolve("io", "print") != nullptr)`（`:50`）就是那条证据：
它的 `io` 参数在新规则下来自文件路径而不是壳名，字符串没变、含义已经换掉。
再补一条**没有壳也应该成立**的对照：`Resolve("math", "sqrt")`。
（`Namespaces()` 这个 API 名要到 Task 7 Step 2 才改叫 `Packages()`，本任务**不要**提前改，
否则 Task 5 的提交会带上 Task 7 的面。）
**如果实测真的不是 38**，先查是不是某行以前因为「不在 namespace 里」被跳过而现在进来了，
把那行的源文本贴进提交说明再改数字——不要先改数字让门闭嘴。

- [ ] **Step 4: 实测冲突数并写回账本**

```bash
win_bison -d --report=all -o /tmp/p.cpp --header=/tmp/p.h src/compiler/grammar/nlang.y
awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}' /tmp/p.output
awk '/[0-9]+ conflicts: [0-9]+ reduce\/reduce/{n+=$4} END{print "rr="(n+0)}' /tmp/p.output
```
期望：第一条命令 **rc=0 且 stderr 为空**（＝`%expect` 仍是 Task 2 bump 后的 16，本任务
**不需要再动它**——「删产生式净效果 ±0」这件事的机械证据就是 `%expect 16` 原样通过），
`awk` 给 `sr=16`、`rr=0`。
（轮 9：原稿这里写「期望 16 shift/reduce」直接读 bison 的退出流，而 Task 1 引入 `%expect` 之后
那条流是空的；测量改走 `--report=all` 的 `.output`，见 Task 1 Step 7 第 3 条。）
删 `namespace` 的净效果是 **±0** 这件事当时的实测形状：`NamespaceMember` 剩 5 个
alternative、`Function` 打头，`Namespace` 产生式与 `%type`/union 成员同删 → 与基线同为
14 sr，因此加回 Task 2 的 +2 就是 16。轮 1 有审阅者预测「14→15，因为两条 sr 是
`Namespace` 产生的」——已被实测否掉，不要再按 15 写账本。账本注释按实际数字改。

- [ ] **Step 5: stdlib 与 fixture 迁移**

`stdlib/{io,math,fs}.n`：删 `namespace <n> {` 与收尾 `}`，成员整体减 4 空格缩进。
公开拼写（`io.print`／`math.sqrt`／`fs.*`）不变——键由路径给出，
`DeriveModulePath` 对 stdlib 文件得到的 stem 恰好等于旧外壳名（D12）。

**轮 6 把「所有内嵌 fixture 字符串同步去外壳」这句换成实测清单**（原稿那句是范围描述，不是
清单；照它 grep 会把 C++ 自己的 `namespace` 一起删掉）。收口命令：

```bash
grep -rn namespace stdlib tests/fixtures tests/test_vm tests/e2e --include='*.n' --include='*.cpp' \
  | grep -vE ':[0-9]+: *(//|\*|using namespace|namespace \{|namespace fs =|\} // namespace)'
```
（`grep -v '^\s*//'` 这种写法在 `-rn` 的输出上是**空转**的：行首是 `文件:行号:`，永远不是空白，
轮 6 实测 7 进 7 出。要筛注释得锚 `:行号:` 之后。）

实测的**去外壳面**（只有这些是要改的源）：
- `stdlib/{io,math,fs}.n`：外壳各 1 行（`:5` 的 `namespace <n> {`）＋各自 `:1` 的
  「the "<n>" namespace」自述注释 ⇒ `grep -rn namespace stdlib` ＝ **6 命中**。
- `tests/test_vm/test_library_source.cpp` ＝ **22 命中**（轮 6 复数，轮 5 记的「20」漏了
  `:39/:46/:48/:433` 这四行 C++ 脚手架），其中**要改的只有 10 行 `.n` 源写串**：
  `:105 :141 :150 :161 :163 :177 :206 :261 :307 :349`；另有 8 行注释措辞（`:158 :198 :200 :202
  :203 :258 :259 :378`）按 Step 6b 的收口 grep 一起清；**4 行不许动**：`:39`
  `using namespace nlang;`、`:46` `namespace {`、`:48` `namespace fs = std::filesystem;`、
  `:433` `} // namespace`——它们是 C++ 自己的语法，与本阶段无关。
- `tests/test_vm/test_thirdparty.cpp` ＝ **7 命中**，其中 `.n` 源写串只有 `:66` 一条
  （`"namespace mylib {\n"`）；`:4 :163` 是注释、`:37 :44 :46 :197` 是 C++ 脚手架。
  轮 5 记的「16 命中」与「36＝20＋16」这条加法整体不成立，实测
  `grep -rn namespace tests/test_vm` ＝ **54 命中**，其中除上面 11 条之外的 43 条全是
  C++ 语法或注释（分布：`test_debugger` 1、`test_library_search_path` 3、`test_native_abi` 3、
  `test_native_host` 4、`test_native_loader` 4、`test_stdlib` 8、`test_strings` 1、`test_vm` 1，
  ＋`test_library_source` 的 12 条非源行＋`test_thirdparty` 的 6 条非源行）。
  `tests/test_vm/test_native_loader.cpp:141` 的 `"missing-module error names the namespace and
  searched paths"` 是 **CHECK 的标签**，不是期望串（`:139-140` 只找 `ghost` 与 `one`），
  Step 6b 改 `NativeLibraryLoader.cpp:125` 的句子**不会**让它红，按注释措辞处理即可。
- **磁盘上的 `.n` fixture 是两份，不是「`tests/fixtures/native/*.n` 同」一句带过**：
  `tests/fixtures/native/mylib/mylib.n:13`（`namespace mylib`）＋
  `tests/fixtures/native/mixlib/mixlib.n:9`（`namespace mixlib`），各自另有 2 行注释
  （`mylib.n:26`、`mixlib.n:4/:16`）。这两份的**消费路径不同**（轮 7 按实测定死；原稿写「经 `ReadFixtureFile` 读进场景目录
  （`test_thirdparty.cpp:196-200`、`:163-167`）」——那两处**都不是读源点**：`:163` 是注释，
  `:196-197` 是 `} // namespace` 脚手架）：
  - `mixlib.n`：**是**读磁盘的。`ReadFixtureFile` 定义在 `test_thirdparty.cpp:153`，
    全文件**只有 `:171` 一个调用点**（读 `MIX_SOURCE_FIXTURE_DIR/mixlib.n`）。
    去外壳要与 `:176-179` 的 `mixlib.quad(3) == 12`／`mixlib.dbl(5) == 10` 同提交：
    包名＝主干（D12），限定名拼写不变，但 `:171` 读到的那份磁盘源必须已是无壳形状。
  - `mylib.n`：磁盘那份**没有 C++ 测试读它**。`TestThirdPartyNativeLibrary` 用内嵌串
    `kMylibSource`（定义 `:65`，写入场景目录 `:132-133`），`:66` 那一行才是本步要改的；
    磁盘 `mylib.n`／`use_mylib.n` 由 **CMake** 消费：`tests/CMakeLists.txt:162-163`
    拷两份进 `e2e_out/mylib_pkg`，三条 ctest 条目 `ncc_thirdparty_run`（`:173`）、
    `ncc_thirdparty_default_run`（`:185`）、`ncc_thirdparty_env_run`（`:198`/`:203`）
    从那里编译＋运行。**mylib 面＝「内嵌串＋磁盘 fixture」两处**，只改内嵌串会让这三条
    ctest 红而单元测试全绿。
  - 顺带清理（轮 7 实测）：`SOURCE_FIXTURE_DIR` 在 `tests/CMakeLists.txt:129` 定义、
    在 `test_thirdparty.cpp:30-31` 只剩 `#ifndef` 回落，**没有任何使用点**——它是 mylib
    改走内嵌串之后留下的死宏，本步连宏带 `#define` 一起删（本仓库不留兼容壳）。
    `MIX_SOURCE_FIXTURE_DIR`（`:33-34`＋使用点 `:171`）是活的，别一起删。
  同目录的 `*_native.cpp` 里的 `using namespace nlang::native;`
  是 C++，不动。
- `tests/e2e/**/*.n` ＝ **7 命中／4 文件**，全部是注释（轮 4 实测），无外壳可删。
- **`examples/` 原本不在任何一条 sweep 里**（轮 6 实测：`grep -rni namespace examples/` ＝
  **4 命中／4 文件**，而 Task 5 的收口 grep 只扫 `stdlib tests tools src docs`）：
  `examples/stdlib_io.n:1`、`examples/stdlib_fs.n:1`、`examples/stdlib_math.n:1` 三条
  「… namespace tour (Phase 11)」自述注释 ⇒ 本步改写成「… package tour」
  （`examples/stdlib_string.n`、`aliases_tour.n` 等其余 20 个文件零命中，别陪跑）。
  为什么要单独点名：`tests/packaging/verify_package.py:118-140` 的
  `scan_public_text(pkg)` 会把 **`examples/` 整个目录**当公开文字面扫（它只在 `docs/site`
  ＋`examples` 上走 `find_violations`），也就是说这批文件是**随包发的**；今天那条门只查
  旧引擎痕迹，查不出「namespace」这个本阶段要清零的概念，所以不能靠门兜。
  `examples/hello_project/hello_project.nproj:2` 的 `namespace="hello_project"` 不在本步——
  它是 Task 7 Step 4 删属性的连带面，跟着那一步走（而且上面命令 (1) 的 `--include='*.n'`
  **根本扫不到 `.nproj`**，所以它不会在本步的收口里露头，也不该在本步收）。
  ⇒ 本步收口就是上面那两条命令，**已经把 `examples` 算进 (1) 了**；不要再另起一把
  `grep -rni namespace <所有目录>`——轮 6 实测那种写法＝1297 命中，见 Files 的收口块。

**`tests/test_compiler/test_module_import.cpp` 的槽处置表**（轮 6 新增：这一面不是「去壳」，
是删语法之后**前提消失**的用例；原稿只在 Files 里留了一句「必须整条重写或删除」，没有说哪几条、
各自动什么）。这个文件是 QtTest（`:404` `private slots:`、文件尾 `QTEST_GUILESS_MAIN`），
删槽不需要动别处——但也**没有别处会替你报警**：槽名里没有 `namespace` 的那些（下表第 1、5 行）
只会红在断言上。

| 槽（行区间） | 它钉的是什么 | 处置 |
|---|---|---|
| `ownerTagsTopLevelAndNamespaceMembers()`（`:798-851`，源字面量 `:810`） | owner 标注覆盖顶层**与嵌套 namespace 成员**两条路径 | **保留槽、删「嵌套 namespace」那一半**：改名 `ownerTagsTopLevelAndMemberNodes`，`namespace NS {` 那层换成 class/struct 成员（同文件末尾的
  `"class widget {\n    public int size;\n"` 内嵌源就是这个形状，见 `:1570`／`:1575`），
  `QCOMPARE(reg.OwnerOf*…)` 的断言不动。「一个容器内的成员各自带 owner」这条事实仍然成立，只是容器从 namespace 换成 class |
| `namespaceCrossTUTagsEachSide()`（`:858-901`，源字面量 `:863`／`:869`） | 两个 TU 各声明同名 `NS` ⇒ 合并成**一个**根节点，成员各带自己 owner | **删除**。闭合模型里同名容器不再可能合并：两个 `.n` 是两个包，同名包路径在同一构建里是 Task 6 Step 5 的构建错误。这条的注释（`:853-857`）描述的「losing shell dies with its unit root／which shell survives follows TU order」正是 D1 要消灭的容器选取行为，随 `CompiledInFunctions`／`FindModuleType` 的 `isLibrary`/`pContainer` 分支一起消失（Step 3） |
| `namespaceNestedCrossTUTagsEachSide()`（`:936-979`，源字面量 `:940/:941/:947/:948`） | 嵌套 `A{B{f}}` 跨 TU 合并 | **删除**，理由同上（`A`／`B` 都不再是可达的容器） |
| `bareNamespaceScopeFiltered()`（`:1368-1405`，源字面量 `:1378`／`:1381`） | 调用点在 namespace **内部**时，裸名池过滤仍要丢掉外来重载（父链遍历那条路） | **删除**。父链上不再有 namespace 节点；「裸名不跨目录命中、提示指名 owner」这一半已由同文件的 `bareCrossDirectoryRejected()`（`:1304`）与 `bareCrossModuleRejected()`（`:1342`）钉住，**不要**为了保数量把它改写成第三个同形用例 |
| `noOwnerForUntaggedNodes()`（`:981-994`，注释 `:978`） | 未标注节点（含「根 namespace 从不作为合并节点」）无 owner | **保留，只改注释**：`NO_OWNER` 语义不变（Task 3 的 D1 就是靠它给内建类型裸名），措辞里的「root namespace」按新事实写成「unit root」 |

删掉三条槽之后本文件的用例数**必然下降**，所以 Step 7 的 ctest 门写的是 **64 条不变**——
条目（`add_test`）与槽数是两回事，别去 `tests/CMakeLists.txt` 找「少了三条」的痕迹。


- [ ] **Step 6: 高亮与文档工具的关键字表**

`highlight.py:24/32` 去 `namespace`，`test_highlight.py:77-90` 的精确关键字集合断言同步。
**`tools/source_size_guard/check_source_size.py` 不在本步范围**（轮 7 驳回原稿的「同」：
那里的 `KEYWORDS` 是 C++ 行分类用的启发式，不是 NLang 关键字表，见 Files 的「不改」条）。

- [ ] **Step 6b: 用户可见文案里的 `namespace` 一词清零（轮 5 新增：这一步原本没人做）**

删掉关键字却留下句子，等于把「namespace」这个已经不存在的概念继续卖给用户——违反本阶段的
公开文字约束。**盘点命令**（本轮实测输出在下面清单里，逐条已核）：

```bash
grep -rn '"[^"]*namespace[^"]*"' src/compiler src/vm src/langservice src/tools \
  --include='*.cpp' --include='*.h' --include='*.hpp' \
  | grep -v 'namespace nlang\|using namespace'
```
命中 **14 行**（轮 6 逐行复跑核对）。先做**减法**再对表，别把 14 抄成「本步 14 条」：

- 4 行不归本步：`SymbolIndex.cpp:99` 的正则（Step 3c 删）、
  `ProjectFile.h:12`／`ProjectModelXml.cpp:245/:284` 的 `.nproj` 属性（Task 7 Step 4 删）。
  ⇒ 剩 **10**。
- 其中 3 行是**保留名表**的文案，本步不动（见下面「剩下 3 处」）：
  `DuplicateFieldChecker.hpp:154`、`StatementResolverDecls.cpp:47`、`ModuleRegistry.cpp:142`。
  ⇒ 本步的 grep 命中＝**7 行**。
- 表却是 **8 行**：前两条 `pKind = "Namespace"` 的 **N 是大写**，上面那条大小写敏感的
  grep 天然扫不到它们（轮 6 实测：把它们并入是故意的，它们是本步最要紧的两条，
  `manifest.txt:1010` 的期望串就是由其中一条拼出来的）。**别照着 grep 输出删表的两行**。

| 位置 | 今天的原文 | 改成 |
|---|---|---|
| `src/compiler/builder/ExprResolverStdLib.cpp:87-88` | `pKind = … ? "Namespace" : "Module"`（`:83-86` 注释里两次出现「library namespace」，同改） | `? "Package" : "Module"` |
| `src/compiler/builder/ExprResolverTypes.cpp:373`（轮 6 实测订正：原稿写 `:371`，那是 `if` 的首行） | `pKind = "Namespace";`（`:368-369` 与 `:363-364` 两段注释里的「namespace」同改） | `"Package"` |
| `src/compiler/builder/ExprResolverTypes.cpp:355` | `Type '%s' is not a member of namespace '%s'.` | `… of package '%s'.` |
| `src/compiler/SnMisc.cpp:78` | `The namespace member "%s" has has already been defined.` | `The package member "%s" has already been defined.`（顺手修 `has has` 这个重复词） |
| `src/compiler/SnMisc.cpp:191` | `The field "%s" is not a namespace.` | `… is not a package.` |
| `src/compiler/builder/ModuleRegistryGate.cpp:83` | `Library namespace '<name>' is indexed but …` | `Library package '<name>' …` |
| `src/compiler/builder/ModuleRegistryGate.cpp:117` | `Wildcard import cannot target library namespace '<name>'.` | `… library package '<name>'.` |
| `src/vm/NativeLibraryLoader.cpp:125` | `… for namespace '<ns>'; searched:` | `… for package '<ns>'; searched:` |
（轮 6 逐行对账：表 8 行＝上一步剩的 6 条句子行＋`ExprResolverTypes.cpp:368` 那条注释行
跟着第 2 行改（它是第 7 条 grep 命中）＋第 1、2 行那两条 grep 看不见的大写 `Namespace`。）

**两条 `pKind` 喂的是两句不同的话**（轮 6 实测，别当成同一句）：
`ExprResolverStdLib.cpp:89-91` ⇒ `"%s '%s' is not imported. Add 'import %s;' at the top of this file."`，
`ExprResolverTypes.cpp:374-376` ⇒ 同一开头但结尾是 `… before using type '%s'."`。


**剩下 3 处不属于本步**：`DuplicateFieldChecker.hpp:154`、`StatementResolverDecls.cpp:47`
（都是 `The name "%s" is reserved for a library namespace.`）与 `ModuleRegistry.cpp:142`
（`… collides with a built-in namespace.`）——它们是**保留名表**的文案，保留名表在
**Task 6 Step 5** 才退役，句子跟着那张表一起消失，本步改了就是改一条马上要删的规则。

**金样本／断言同步（漏一条门就红，且红因看着像「实现错了」）**：
- `tests/e2e/manifest.txt:1010` 逐字写着 `Namespace 'io' is not imported. Add 'import io;' at
  the top of this file.` ⇒ 改成 `Package 'io' …`（这条只有 `run_e2e_tests.py` 会跑，见 Step 7）。
- `tests/test_compiler/test_module_import.cpp:1450-1452` 的 `builtinNamespaceRequiresImport()`
  拼的是**整句** `std::string("Namespace '") + szNs + "' is not imported. Add 'import " …` ⇒ 同步；
  函数名 `builtinNamespaceRequiresImport` 与 `:1427-1428` 的注释（"built-in namespaces are gated
  … with the namespace wording"）一起按新事实改写。
- `tests/CMakeLists.txt:1020` 的 `PASS_REGULAR_EXPRESSION "collides with a built-in namespace"`
  **本步不动**（它测的就是 Task 6 才删的那条），但要在提交说明里点名它仍然钉旧词，
  免得下一个任务以为漏改。
- **公开文字里逐字引用这句的 6 行**（轮 6 实测：`grep -rn "Namespace '" docs/user_manual/en docs/user_manual/zh` ＝
  6 命中，en/zh 各 3；大小写敏感的 `grep -rn namespace` **看不见它们**，Task 8 的盘点就漏在这一条）：
  `docs/user_manual/en/getting-started/function-features.md:125`、`docs/user_manual/en/language-spec/common-errors.md:66`、
  `docs/user_manual/en/language-spec/declarations.md:64`、`docs/user_manual/zh/getting-started/function-features.md:117`、
  `docs/user_manual/zh/language-spec/common-errors.md:61`、`docs/user_manual/zh/language-spec/declarations.md:57`
  ⇒ 全部 `Namespace 'io'` → `Package 'io'`。这三对是**引用编译器发射的整句**，跟着句子走，
  不归 Task 8（Task 8 的门只核 en/zh 配对与链接，看不出句子已经不存在）。`git add` 跟着带上 `docs/`。
- **一处「已知限制」整条作废＋两处 FAQ 段要改写**（轮 6 立，轮 7 按实测把**行界与手法**分开定死——
  原稿把四类命中一律写成「删除」，其中两类的行段里混着**仍然成立**的句子，照原稿删会删掉有效文档）：
  - **可整条删除的**：`docs/user_manual/en/language-spec/declarations.md:47-53`（7 行，从
    `- Known limitation:` 到 `for them.`）＋ `docs/user_manual/zh/language-spec/declarations.md:43-46`
    （4 行，从 `- 已知限制：` 到 `无效。`）。轮 7 实测：zh 这一段是 **43-46**，不是原稿写的
    `43-47`——`:47-48` 起是另一条仍然成立的「导入目标的解析顺序：内建 → 项目文件 → 外部
    `.nmod`（经 `-I`）。没有隐式回退。」，
    多删一行就把解析顺序这条规则从 zh 手册里抹掉了。**而且没有门会响**：`tools/nlang-docs/tests/`
    的七条测试里唯一管双语的那条（`test_tree_parity.py`）只比**页面清单与两份 nav**，不比正文行，
    所以「en 留着／zh 删多一行」这种失衡是**静默的**——只能靠这里写死的行界防住。
  - **只能改写的**：`docs/user_manual/en/getting-started/faq.md:73-81` 与 `docs/user_manual/zh/getting-started/faq.md:60-65`
    各是**一个段落**，段首半句在删除语法之后依然正确（en `:73-74`「`import` only opens
    **qualified names** — after `import lib;` you must write `lib.f()`; the bare name `f()`
    does not resolve」、zh `:60-61`「`import` 只开放**限定名**——……裸名 `f()` 不解析」），
    要拿掉的只是中间那句共享命名空间的可达性限制——**它跨在行中间**：en 起于 `:74` 行内
    （`Members of a cross-directory **shared namespace**`，该行的前半属于保留句）、止于 `:77`
    行内（`…nor a qualified form.`，该行后半的 `The visibility` 属于保留句）；zh 起于 `:61`
    行内（`；跨目录**共享命名空间**`）、止于 `:63` 行内（`限定形式。`之后的「各引用形式的
    可见性规则见」保留）。段落其余的指路句照留。
    ⇒ 手法＝**把中间那句剪掉并把前后接起来**，改写后 en 约 73-78 行、zh 约 60-63 行；
    **不是删整段**。
  - **两条收口 grep（`d7ca710` 实测基线，改写前先跑一遍确认自己在同一张图上）**：
    ```bash
    grep -rniE "shared namespace|namespace shared|共享(的)?命名空间" docs/user_manual/en docs/user_manual/zh   # 4 命中 → 0
    grep -rn "only opens\|只开放" docs/user_manual/en docs/user_manual/zh                                      # 4 命中 → 4
    ```
    第一条的四行＝本步要清的两个面（en/zh declarations 的整条删除面＋en/zh faq 的改写面）；
    注意 zh declarations 的原句写作「跨目录共享**的**命名空间」，所以 pattern 里的 `(的)?`
    不能省（轮 7 就是靠这条把 zh 那一行捞回来的）。第二条的四行是**别的页**里仍然成立的
    「import 只开放限定名」（faq en/zh ＋ `common-errors` en/zh），本步之后必须**一行不少**——
    它测的就是「改写有没有顺手删掉有效句」。
  删关键词之后 `namespace NS` 这个写法**根本构造不出来**（不是合法源），而闭合模型里两个文件
  也不可能共用一个包名——同路径的两个来源在 Task 6 Step 5 直接是构建错误。本步删／改完句子，
  Task 6 Step 5 的新规则（同一次构建内包名重复 ⇒ 指名两个来源路径）由 Task 6 自己补进
  `declarations.md`。排在 Task 5 而不是 Task 6 的理由：句子在 Task 5 结束时就已经在描述一种
  不存在的语法，等到 Task 6 会让两个提交各自带着一份假手册。
- 其余命中 `Namespace`/`namespace` 的**测试内注释**按 Step 5 的收口 grep 一起清掉。

- [ ] **Step 7: 全量＋文档门**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
D:/dev/miniconda3/python.exe tests/e2e/run_e2e_tests.py \
  build-dev/src/tools/ncc/Release/ncc.exe build-dev/src/tools/nvm/Release/nvm.exe \
  | tee /tmp/e2e.log; grep -c '^SKIP' /tmp/e2e.log      # 期望：0
PYTHONPATH=tools/nlang-docs/src NLANG_NCC=$PWD/build-dev/tests/Release/ncc.exe \
NLANG_NVM=$PWD/build-dev/tests/Release/nvm.exe \
D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests -q
```
期望 **64/64**（门限见 Global Constraints：Task 4 起 64）＋ e2e **977 passed / 6 failed**（974 基线＋Task 1 的三条 `qhead_*`；六条既有失败
逐字见 Task 1 Step 2 的块，本任务只改 `manifest.txt:1010` 的期望串、不加行）＋ 67 passed
（docs 门实测值，轮 6 复跑确认）。`e2e_compile`／`project_run`／`test_native_*` 是本步的锚。

> **轮 5 补（本任务原先没有 e2e 门，违反 Task 4 Step 11 立下的规则）**：Task 5 的门必须带
> `run_e2e_tests.py`，理由是一条具体用例：`tests/e2e/stdlib_sqrt_canary.n`（`manifest.txt:774`）
> 走 `import math;` ＋ `math.sqrt(4.0)` 的**整条管道**（resolver 拦截 → `OP_CallIntrinsic` 产码 →
> `.nmod` → VM 执行），而 Step 5 删的正是 `stdlib/math.n` 的外壳。这条 canary 只在 e2e runner 里
> 跑（`manifest.txt` 不在 ctest，见 Task 4 Step 11 的轮 4 实测），没有这道门就可能「ctest 绿着把
> 半截 stdlib 提交」。实测（轮 7 纠正计数，原稿写「4 个命中」是把**文件数**记成了**命中数**）：
> `grep -rn namespace tests/e2e --include='*.n'` ＝ **7 命中／4 文件**，且**全在注释里**
> （`import_io_missing.n:2`、`stdlib_reserved_names.n:1/5/6/8`、`stdlib_sqrt_canary.n:1`、
> `unresolved_import_call/lib.n:2`），没有真正的 `namespace` 声明，所以本任务对 e2e 的改动是
> 「注释措辞＋可能的期望串」，文件集合不变——但 canary 的行为面必须由 runner 证明。
> 轮 7 另记：这 7 行里只有 `stdlib_reserved_names.n` 的 4 行会被 Step 5 的清理碰到，而 Step 5 用的
> 两条 pattern（见该步的坑①）**都匹配不到它**——命中的是上面这条无 `-w`、无引号约束的宽 grep。
> 别把这条宽 grep 的数当成 Step 5 两条 pattern 的收口证据。

- [ ] **Step 8: Commit**

```bash
git add src/compiler/grammar src/compiler/builder src/compiler src/langservice \
        src/vm stdlib tests tools docs
git commit -m "feat(lang): remove the namespace keyword; the file's path is its package"
```
`src/compiler/builder`＝Step 3b 的容器判定，`src/langservice`＝Step 3c 的索引来源，
**`src/compiler`（根，收 `SnMisc.cpp`）与 `src/vm`（收 `NativeLibraryLoader.cpp`）＝轮 5 补的
Step 6b 文案面**——原稿的路径清单只有 `src/compiler/grammar` 与 `src/compiler/builder` 两个
**子目录**，`src/compiler/SnMisc.cpp` 这个文件不在任何一个里，`src/vm` 更整个没列；
漏任何一个都会把跨任务的改动留在树里（`git status --porcelain` 除 Global Constraints 记的
基线行 `?? main.n` 之外必须空——轮 7：原稿写「必须空」，而这道门在本工作树里永远红，
永远红的门等于没有门）。
**`docs` 是轮 6 补的**：Step 6b 的清单收了 6 行逐字引用 `Namespace 'io' …` 的手册行
（en/zh 各 3 对），原稿的清单里没有 `docs`，这 6 行会留在树里等 Task 8——而 Task 8 的盘点是
小写 grep，看不见大写 `Namespace`，等于没人认领。

---

### Task 6: 带点 `import` 落地——库包名由搜索根相对派生

**Files:**
- Modify: `src/compiler/ModuleBuilderImports.cpp:100-111`（`FindLibrarySourceFile` —— **函数体只到
  `:111`，轮 3 实测；它不接 `BuildEnvironment` 参数，搜索目录来自成员 `m_upEnv`，
  Step 3 新写的循环要用 `EffectiveLibraryDirs(m_upEnv->Params())`（`:33`）就得把目录显式传进去
  或改成成员内联，别照抄一个不存在的形参**）、`:116-137`（`ParseLibraryUnit`，签名要加匹配根）、
  `:145-187`（`DiscoverLibraryUnits`：**通配符
  `continue` 在 `:171-172`，带点名的 `continue` 在 `:178-179`，删的是后一条**）、
  `:279-289`（`FindModuleFile`）
- Modify: `src/compiler/ModuleBuilder.cpp:233-258`（`MergeTransUnits` —— **轮 3 勘误＋轮 4 复测：
  原稿写的 `ImportedNamespaces()` 在本仓库零命中（`grep -rn ImportedNamespaces src include`
  空），跨 TU 合并的真实函数是 `MergeTransUnits`（定义 `:233`），它的调用者是
  **`ResolveAll()`（`:85`）在 `:89`**，不是 `Build()`；`ModuleBuilder.cpp:129-155` 是
  `RegisterUnits`，见下一条，两处别再混）**
- Modify: `src/compiler/builder/ModuleRegistry.cpp:48-65`（`DeriveModulePath`）、`:71-98`（`FindReservedSegment`／`IsReservedLibraryName`）、`:100-105`（`ModuleNotFoundText`，**轮 4 实测：声明在 `ModuleRegistry.h:33`、定义在 `ModuleRegistry.cpp:100`；`:38` 声明的是 `IsReservedLibraryName`，原稿的 `:91` 是 `FindReservedSegment` 的行**）、`:107-152`（`RegisterUnit`，保留名唯一调用点 `:138`）
- Modify: `src/compiler/builder/ModuleRegistry.h:33`（`ModuleNotFoundText`）＋`:35-38`（保留名 API）＋调用点
- Modify: `src/compiler/ModuleBuilder.cpp:129-155`（`RegisterUnits` 拿到匹配根；
  **阶段顺序轮 4 实测**：`PrepareUnits()`（`:59`）里 `DiscoverLibraryUnits():71` →
  `RegisterUnits():74`，`ResolveAll()`（`:85`）里 `MergeTransUnits():89`，`Build()`（`:98`）
  只是这两个阶段的调用者——`MergeTransUnits` 不在 `Build()` 体内）
- Test: `tests/test_vm/test_library_source.cpp`、`tests/test_vm/test_thirdparty.cpp`、`tests/test_compiler/test_module_import.cpp`
- Modify: `docs/user_manual/en/language-spec/declarations.md:56-58`／`:63-68`（块内 `:67` 删死句）、
  `docs/user_manual/zh/language-spec/declarations.md:49-51`／`:56-61`（`:60` 删死句）——**轮 6 新增**，
  Step 3 与 Step 5 各自把这两页里的一条规则变成假的，理由与逐行内容见 Step 6 第 3 条

- [ ] **Step 1: 写失败的测试**（轮 4 补全：原稿这里只有三条**签名**＋一句「每条给完整源」，
> 那是占位符；并且漏了两条设计矩阵点名要的——§4 三段路径与 §9／D5 的多段包 `native` 诊断，
> 后者按 Task 6 Step 6 的说法「D5 的 native 诊断保持 Task 4 的写法」（诊断本体在 **Task 4 Step 10**
> 末尾，`FillNativeFunctionRecord` 调用侧，`VmBackend.cpp:133-136`；**轮 5 纠正**：原稿写成
> 「Task 4 Step 6」，那一步是重复键诊断，与 native 无关），可 Task 4 的单段场景凑不出多段包，
> 于是全阶段没人钉它。现一并补在这里。）

**落点选 `tests/test_vm/test_thirdparty.cpp`**（不是 `test_library_source.cpp`）：只有它的
入口 `buildAndRun`（`:93-123`）自己清并塞 `m_ImportDirs`（`:101-102`，今天只塞 `pkg` 一个），
带点导入与双根撞名都要多个根。改动方式给 `buildAndRun` 加一个**带默认值**的根列表
（`const std::vector<std::string>& importDirs = {}`，空＝沿用 `{pkg}`），
别造第二个 build 入口。

**负例要读诊断文案，而「再编译一次拿文案」在这份 harness 里是错的**（轮 6 实测，
原稿的 `compileLogOf(pkg, dirs)` 单独存在就会踩这个坑）：`buildAndRun` 的模块名＝场景目录名
（`:94`），`ModuleManager::Create` 对同名第二次调用返回 0（`src/runtime/Module.cpp:66-70`
`module %s already exists`，进程内没有 unload API），于是**同一目录的第二次 `Build()`
必然失败**，拿到的文案是「create module failed」而不是被测诊断。所以本任务把 build 那半段
抽成**一次编译、两个结果**，`buildAndRun` 与所有负例共用它：

```cpp
struct ScenarioBuild {
    bool ok = false;
    std::string log;   //every CLL_* message, concatenated
};
//One compile per scenario dir: a second Build() on the same dir name would be
//refused by ModuleManager::Create (process-wide, no unload), and its log would
//be about that collision instead of about the test.
ScenarioBuild compileScenario(const fs::path& pkg,
                              const std::vector<std::string>& importDirs);
```
`buildAndRun(pkg, io, importDirs)` ＝ `compileScenario` ＋（`ok` 时）load/run，
把 `log` 逐条 `std::fprintf("diag: %s")` 打出去（保留今天 `:113-114` 的可诊断性）；
负例一律写成 `ScenarioBuild r = compileScenario(dir, dirs); CHECK(!r.ok, …);
CHECK(r.log.find(…), …);`——**永远不要在同一个场景目录上先 `buildAndRun` 再 `compileScenario`**。
下面 `(2)`～`(5)` 吃的都是它。


```cpp
//(1) R3 正例：`-I <根>` ＋ `<根>/vendor/graphics.n` ⇒ `import vendor.graphics;`
//    ＋ `vendor.graphics.hue()` 通。本任务在 Task 5 之后，`namespace` 产生式已经删掉，
//    所以这里**不能**再写壳（轮 5：原稿的 `namespace wrong {…}`＋「按路径纠正壳」
//    是 D8 的写法，D8 由 Task 4 Step 1b `(1)` 在删语法**之前**拥有，别在这里重排一遍）。
static void TestDottedLibraryImport() {
    const fs::path pkg = packageDir() / "dotted_import";
    fs::create_directories(pkg / "vendor");
    std::ofstream(pkg / "vendor" / "graphics.n", std::ios::binary)
        << "struct Sprite { int id; }\n"
           "int hue() { return 3; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import vendor.graphics;\n"
           "int main() {\n"
           "  vendor.graphics.Sprite sp;\n"
           "  sp.id = vendor.graphics.hue();\n"
           "  return sp.id - 3;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(pkg, io, { pkg.string() }) == 0,
          "a dotted import resolves through <root>/vendor/graphics.n");
}

//(2) R3 负例：末段简写不是限定拼写。轮 6 实测后拆成两条，各自钉一句真话——
//    原稿的 `not found || not imported` 是**空转断言**（两种文案任选其一都算过，
//    Step 5 换判据时改坏了也绿）。
//    (2a) `import graphics;`：今天实测已经是
//         `Error: Module 'graphics' not found. Check the project Sources list or -I import path.`
//         ⇒ 这是一条**回归护栏**（Step 2 里它不红，别按「先红后绿」理解它）。
//    (2b) 带点 import 成功之后，末段简写 `graphics.hue()` 仍然不是可用拼写：
//         它依赖 (1) 先绿，是真正的新行为。断言吃 `Module 'graphics'` 这段——
//         `ModuleNotFoundText`（`ModuleRegistry.cpp:100-105`）与 not-imported 文案
//         （`ExprResolverStdLib.cpp:89-91`，pKind 为 `Module` 时）都以它开头，
//         所以判据换到哪一边都仍然指到名字。
static void TestLastSegmentShorthandRejected() {
    const fs::path root = packageDir();     //exactly ONE call: see the note below
    const fs::path imp = root / "dotted_short_imp";
    const fs::path use = root / "dotted_short_use";
    for (const fs::path& pkg : { imp, use }) {
        fs::create_directories(pkg / "vendor");
        std::ofstream(pkg / "vendor" / "graphics.n", std::ios::binary)
            << "int hue() { return 3; }\n";
    }
    std::ofstream(imp / "prog.n", std::ios::binary)
        << "import graphics;\n"
           "int main() { return graphics.hue(); }\n";
    std::ofstream(use / "prog.n", std::ios::binary)
        << "import vendor.graphics;\n"
           "int main() { return graphics.hue(); }\n";
    //(2a) shorthand import
    ScenarioBuild a = compileScenario(imp, { imp.string() });
    CHECK(!a.ok, "importing the last segment alone fails the build");
    CHECK(a.log.find("Module 'graphics'") != std::string::npos,
          "the shorthand import is rejected by that exact name");
    //(2b) real dotted import, shorthand use
    ScenarioBuild b = compileScenario(use, { use.string() });
    CHECK(!b.ok, "a bare last segment is not callable after the dotted import");
    CHECK(b.log.find("Module 'graphics'") != std::string::npos,
          "and the diagnostic names the shorthand, not the real package");
}
```
> **轮 6 实测的两条 harness 规矩，形状就是这么定的**：
> - **`packageDir()` 每调一次就 `fs::remove_all` 掉整个共享根**
>   （`tests/test_vm/test_thirdparty.cpp:55-62`，注释自己写着 "Start from a clean slate"）⇒
>   它擦的是**所有**场景，不只是它返回的那个名字。所以规矩是「**先跑完再派生下一个**」：
>   一个场景的写入＋`buildAndRun`/`compileScenario` 必须整体发生在下一次 `packageDir()`
>   之前。`(2)` 因为要在两个目录名之间共用同一份 `vendor/graphics.n`，选择调一次、
>   用 `root / "…"` 派生；`(4)` 三个场景各自交错调用，也合规。既有文件本来就是后一种
>   形状（`:129` 与 `:167` 各调一次，`main()` 顺序跑）。
> - **一次编译＝一个目录名，负例不许再编第二遍**。模块名＝`pkg.filename()`（`:94`），
>   `ModuleManager::Create` 对同名第二次调用返回 0（`src/runtime/Module.cpp:66-70`，
>   `module %s already exists`，进程内无 unload API）⇒ `CreateModule` 失败
>   （`src/compiler/ModuleBuilder.cpp:157-164`）⇒ `Build()` 返回 false。同目录跑第二次
>   ＝**以错误的理由让负例断言变绿**（日志是「create module failed」，不是被测诊断），
>   所以上面把 build 抽成 `compileScenario` 一次给出 `ok`＋`log`，负例不再需要第二次编译。



```cpp

//(3) D4：两个搜索根各自给出包 `graphics` ⇒ 指名两个来源路径的诊断，不是 first-wins。
static void TestDuplicatePackageAcrossRoots() {
    const fs::path pkg = packageDir() / "dup_roots";
    fs::create_directories(pkg / "r1");
    fs::create_directories(pkg / "r2");
    std::ofstream(pkg / "r1" / "graphics.n", std::ios::binary)
        << "int hue() { return 1; }\n";
    std::ofstream(pkg / "r2" / "graphics.n", std::ios::binary)
        << "int hue() { return 2; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import graphics;\nint main() { return graphics.hue(); }\n";
    ScenarioBuild r = compileScenario(pkg, { (pkg/"r1").string(), (pkg/"r2").string() });
    CHECK(!r.ok, "two roots offering the same package name fail the build");
    CHECK(r.log.find("graphics") != std::string::npos, "the diagnostic names the package");
    CHECK(r.log.find("r1") != std::string::npos && r.log.find("r2") != std::string::npos,
          "both source paths appear (Step 5's new duplicate-package check)");
}

//(4) 设计 §4：三段目录路径可拼可跑，且 `import gfx.color;` 不给出 `gfx.color.deep`。
static void TestThreeSegmentPackageAndSiblingInvisibility() {
    //(4a) 正例
    const fs::path ok = packageDir() / "three_seg";
    fs::create_directories(ok / "gfx" / "color");
    std::ofstream(ok / "gfx" / "color" / "deep.n", std::ios::binary)
        << "struct Shade { int v; }\n"
           "int tone() { return 4; }\n";
    std::ofstream(ok / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\n"
           "int main() {\n"
           "  gfx.color.deep.Shade s;\n"
           "  s.v = gfx.color.deep.tone();\n"
           "  return s.v - 4;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(ok, io, { ok.string() }) == 0,
          "a three-segment library path is addressable for types and functions");
    //(4b) 负例：父段 import 不给子段（D2 的可见性只在真正那一层）。
    //    轮 6 实测订正：原稿这里只放了 `gfx/color/deep.n`，而闭合模型里**目录不是包**——
    //    没有 `gfx/color.n` 这个文件，`import gfx.color;` 就是一条「模块不存在」，
    //    今天与实现之后**都**只会报 `Module 'gfx.color' not found.`（实测 ncc 实跑），
    //    那句里根本没有 `gfx.color.deep` 这个串 ⇒ 断言在正确的实现上也是红的。
    //    补一份真实的父包 `gfx/color.n`，这条才变成「父可见、子不可见」的可判伪形状：
    //    失败必须来自 `gfx.color.deep` 这一层的可见性，而不是来自一个不存在的模块名。
    const fs::path ng = packageDir() / "three_seg_neg";
    fs::create_directories(ng / "gfx" / "color");
    std::ofstream(ng / "gfx" / "color.n", std::ios::binary)
        << "int base() { return 1; }\n";
    std::ofstream(ng / "gfx" / "color" / "deep.n", std::ios::binary)
        << "int tone() { return 4; }\n";
    std::ofstream(ng / "prog.n", std::ios::binary)
        << "import gfx.color;\n"
           "int main() { return gfx.color.base() + gfx.color.deep.tone(); }\n";
    ScenarioBuild neg = compileScenario(ng, { ng.string() });
    CHECK(!neg.ok, "the parent package does not open the child package");
    CHECK(neg.log.find("gfx.color.deep") != std::string::npos,
          "the missing-import diagnostic names the exact child package");


    //(4c) 设计 §11 的另一半（轮 5 从 Task 2 Step 7 搬来）：包真的存在、类型不在包里。
    //    Task 2 凑不出这个前提（带点 import 要到本任务才落地），所以钉子排在这里。
    const fs::path miss = packageDir() / "three_seg_missing_type";
    fs::create_directories(miss / "gfx" / "color");
    std::ofstream(miss / "gfx" / "color" / "deep.n", std::ios::binary)
        << "struct Shade { int v; }\n";
    std::ofstream(miss / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\n"
           "int main() { gfx.color.deep.NoSuch t; return 0; }\n";
    ScenarioBuild missOut = compileScenario(miss, { miss.string() });
    CHECK(!missOut.ok, "a package without that type is not silently accepted");
    CHECK(missOut.log.find("is not a member of") != std::string::npos
          && missOut.log.find("gfx.color.deep") != std::string::npos,
          "an imported package without that type says so, naming the package");
    //只钉 `is not a member of` 这段稳定前缀：整句在 Task 5 Step 6b 从
    //`not a member of namespace` 改成 `not a member of package`，逐字钉会跨任务红。
}

//(5) D5／设计 §9：多段包里写 `native` 是编译期诊断（DLL 名按单段包名拼，
//    `nlang_gfx.color.dll` 这种串不是任何真实文件）。诊断在 Task 4 Step 10 落地，
//    本条是它唯一可能的钉子——Task 4 的单段场景造不出多段包。
static void TestNativeInMultiSegmentPackageRejected() {
    const fs::path pkg = packageDir() / "native_multi";
    fs::create_directories(pkg / "gfx" / "color");
    std::ofstream(pkg / "gfx" / "color" / "deep.n", std::ios::binary)
        << "native int tone();\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\nint main() { return gfx.color.deep.tone(); }\n";
    ScenarioBuild nat = compileScenario(pkg, { pkg.string() });
    CHECK(!nat.ok,
          "the build actually fails - a message alone would also match a green run");
    CHECK(nat.log.find("native") != std::string::npos,
          "the diagnostic mentions native");
    CHECK(nat.log.find("gfx.color.deep") != std::string::npos,
          "and it names the package that cannot carry it");
}
//(6) 设计 §2 同末段两包共存（轮 5 补：§2 原本只在 Task 3 Step 2 留了一句「要到 Task 6
//    Step 1 才凑得出带点 import」的指针，这里就是那个落点；不补上，Self-Review 的
//    「§1～§15 逐条对得上」就是假话）。`a.io` 与 `b.io` 末段相同、前缀不同，
//    函数与类型都必须各归各的包。
//    轮 6 订正：原稿的函数侧是 `a.io.f()`＋`b.io.g()`（**两个不同名**），键要是串了
//    也照样各查各的 ⇒ 函数侧根本不可判伪。现在两侧都叫 `f()`、返回不同值，
//    同一形状下串键＝两个调用拿到同一个函数＝和不对＝红。类型侧原稿已经是
//    同名不同布局（`Rec{ x }` vs `Rec{ y, z }`），保持。
static void TestSameLastSegmentPackagesCoexist() {
    const fs::path pkg = packageDir() / "same_last_seg";
    fs::create_directories(pkg / "a");
    fs::create_directories(pkg / "b");
    std::ofstream(pkg / "a" / "io.n", std::ios::binary)
        << "struct Rec { int x; }\n"
           "int f() { return 10; }\n";
    std::ofstream(pkg / "b" / "io.n", std::ios::binary)
        << "struct Rec { int y; int z; }\n"   //同类型名＋不同布局，串了就会算错
           "int f() { return 20; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import a.io;\n"
           "import b.io;\n"
           "int main() {\n"
           "  a.io.Rec p; p.x = 7;\n"
           "  b.io.Rec q; q.y = 1; q.z = 2;\n"
           "  if (p.x != 7 || q.y + q.z != 3) return 1;\n"
           "  if (a.io.f() + b.io.f() != 30) return 2;\n"
           "  return 0;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(pkg, io, { pkg.string() }) == 0,
          "two packages sharing a last segment coexist; functions and types stay separate");
}
```
六条都要注册进 `test_thirdparty.cpp` 的 `main()` 调用串（手写 `CHECK` 风格，
**不是**自动发现）。

> **`(6)` 用 `io` 当末段是故意的，别改成别的名字**：`io` 既是内置库名、又是
> `a.io`／`b.io` 的末段，这条同时钉住「两段路径与内置单段 `io` 不是同一身份」。
> 如果它撞在 `IsReservedLibraryName`／`IsLibraryNamespace` 上（保留名那条规则本任务
> Step 5 才退役，`RegisterUnit` 的 `FindReservedSegment` 只判**项目**目录、
> `ModuleRegistry.cpp:136` 那层 `isLibrary` 豁免已经写在码里），**报出来讨论**，
> 不要靠换名绕开——换名等于把这条用例的第二个目的删掉。

> **`(2)` 的断言口径（轮 6 重写）**：原稿的 `not found || not imported` 是**或**出来的
> 空转断言，两条都不许留。现在两条各钉一句 `Module 'graphics'` —— 这个片段是
> `ModuleNotFoundText`（`ModuleRegistry.cpp:100-105`）与 not-imported 文案
> （`ExprResolverStdLib.cpp:89-91`，pKind＝`Module`）的**共同前缀**，所以 Step 5 换判据
> 之后仍然指到被测名字；而逐字钉整句会跨任务红（`Package`/`Namespace` 名词在 Task 5
> Step 6b 才换）。真正钉整句的工作在 Step 6 的文案表里，别在这里重复钉。


**`(4a)`／`(4b)` 的语法前提轮 4 实测过**（写单文件 `namespace A { namespace B { struct S
{int v;} } }` ＋ `A.B.S v;`，ncc 实跑）：三段类型名在**声明位**是**解析通过的**，报的是
resolver 层的 `Module 'A.B' is not imported. Add 'import A.B;' ... before using type 'S'.`
——不是 `syntax error`。所以本条不需要新语法工作，Task 1 的范畴检查也不必为声明位再加东西；
`(4b)` 的断言吃的就是这条既有文案的模块名部分。

- [ ] **Step 2: 跑它们，逐条对照下面的期望**（阻塞点在
`src/compiler/ModuleBuilderImports.cpp:178-179`——`DiscoverLibraryUnits` 对带点名直接
`continue`，注释自己写着「Dotted names resolve as project modules, not package files」）

轮 6 实测／推演出来的**逐条**期望，别写「全部失败」交差：

| 用例 | 今天 | 原因 |
|---|---|---|
| `(1)` | 红（`buildAndRun != 0`） | `import vendor.graphics;` 走到 `continue` ⇒ `Module 'vendor.graphics' not found` |
| `(2a)` | **绿** | 这是**回归护栏**：`import graphics;` 今天就是 `Module 'graphics' not found…`，Step 5 换表后必须仍是这个形状。按「先红后绿」理解它会误判 |
| `(2b)` | 红 | 今天的日志是 `Module 'vendor.graphics' not found`，**不含** `Module 'graphics'`（少了引号对齐）⇒ 第二条 CHECK 红；第一条 `!b.ok` 今天以错误的理由绿，所以**两条都要** |
| `(3)` | 红 | 两个根各有一份 `graphics.n`，今天 first-wins 编过 ⇒ `r.ok` 真 |
| `(4a)`／`(6)` | 红 | 同 `(1)` |
| `(4b)` | 红 | 今天 `import gfx.color;` 也走 `continue` ⇒ 日志只有 `Module 'gfx.color' not found.`（实测 ncc 同一形状，且 `gfx.color.deep.tone()` 那一层**没有**再发第二条诊断）⇒ 含 `gfx.color` 但不含 `gfx.color.deep` |
| `(4c)` | 红 | 日志含 `gfx.color.deep`，但**不含** `is not a member of`（还没到类型判定那层） |
| `(5)` | 红 | 日志含 `gfx.color.deep`，不含 `native`（还没走到产码） |

`(4b)`／`(4c)`／`(5)` 三条的**第一条 CHECK（`!ok`）今天是绿的**（因为编译以别的理由失败），
这正是把它们合进同一个测试函数的理由：红的一定是**指名道姓那一条**。跑完把观察到的实际
日志贴进提交说明，尤其 `(4b)` 若今天因为 `gfx` 被当成目录名而给出别的句子，就按句子校正
本表而不是改断言（本任务还没实现，改断言要写明理由）。


- [ ] **Step 3: 点分名找源码，并把匹配到的根交回**

`FindLibrarySourceFile` 换成「点分名 → 相对路径」并返回 `(path, matchedRoot)`：

```cpp
//Locate a library source for a dotted package name: `a.b.c` is
//<root>/a/b/c.n on the first matching search dir. Returns the file and the
//root it was found under, because the package name is the path RELATIVE TO
//THAT ROOT — a bare stem would make vendor/graphics.n register as `graphics`
//and be unreachable as `import vendor.graphics;`.
std::pair<std::string, std::string> ModuleBuilder::FindLibrarySourceFile(
	const std::string& dottedName) const
{
	std::string rel = dottedName;
	std::replace(rel.begin(), rel.end(), '.', '/');
	for (const auto& dir : EffectiveLibraryDirs(m_upEnv->Params()))
	{
		const std::string path = dir + "/" + rel + ".n";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return { path, dir };
	}
	return { };
}
```
`DiscoverLibraryUnits`：删掉 `:178-179` 的 `continue`，把匹配根随路径一起传给
`ParseLibraryUnit(path, root)`，后者把 `(absPath → root)` 记进新表
`m_librarySourceRoots`（`ModuleBuilder.h`）。`ParseLibraryUnit` 的递归固定点语义不变。

- [ ] **Step 4: 注册时用匹配根算包名**

`RegisterUnits`（`ModuleBuilder.cpp:137-153`）改为：库 TU 传匹配根作为 `projectDir`
参数（`DeriveModulePath(file, matchedRoot)` 于是给出 `vendor.graphics`）；项目 TU 仍传
`m_sProjectDir`。签名不必变——`RegisterUnit` 的第三参数就是「相对起点」，注释里把它
改名为 `packageRoot` 并说明两种起点。

- [ ] **Step 5: 保留名表退役，改撞名诊断（ctest 条目改名，总数不变）**

删 `IsReservedLibraryName`／`FindReservedSegment`（`ModuleRegistry.cpp:71-98`、
`ModuleRegistry.h:35-38`）及其在 `RegisterUnit` 里的调用（**唯一调用点是
`ModuleRegistry.cpp:138`，诊断文案在 `:142`**）。原稿说的「`ModuleRegistryGate.cpp` 里以保留名为
由的分支」经轮 2 实测**不存在**（该文件里没有任何 reserved 字样），别再去找。

撞名诊断改为「本构建内包名重复 → 指名两个来源路径」。三处钉子必须一起拔：
1. `tests/test_compiler/test_module_import.cpp:468-472`（按 `"namespace."` 文案断言的用例）；
2. **`proj_reserved_segment_compile` 不删，改造成重复包名用例**（轮 2 纠正原稿）：
   条目在 `tests/CMakeLists.txt:1013-1020`，`PASS_REGULAR_EXPRESSION` 现钉
   `collides with a built-in namespace`，fixture 是
   `tests/e2e/proj_reserved_segment/{io/, main.n, proj_reserved_segment.nproj}`
   （`main.n` 只有 `int main() { return 0; }`，触发点是目录名 `io`）。
   保留名概念消失后这条没有存在理由，但**删条目会把总数从 64 变成 63**（Task 4 Step 8c 之后
   门限已经是 **64**，见 Global Constraints），而 Task 6/7/8 的期望值写的都是当时的门限值——
   少一条就是口径自相矛盾（轮 2 审核就是从这里找出来的，当时它表现为「63 变 62 而别处写
   63/63」）。
   **决定：改名复用**，条目更名 `duplicate_package_path_compile`，`.nproj` 的 Sources 列
   两份**同相对路径**的库源（做法：`-I` 传两个库根 `roots/r1` 与 `roots/r2`，各自放一份
   `graphics.n`，两边按匹配根派生都得到包 `graphics`），fixture 里写 `import graphics;`，
   `PASS_REGULAR_EXPRESSION` 改钉新诊断里的**两个来源路径**片段。
   `tests/e2e/proj_reserved_segment/` 目录整体换成新 fixture 目录，别留下空壳。
   **改名要同时拔两处，不是一处**（轮 7 补，原稿只写了条目本体）：
   `tests/CMakeLists.txt:1015-1018` 的 `add_test(NAME …)` 与 `-o …/e2e_out/proj_reserved_segment.nmod`
   输出路径之外，`:1034-1039` 的
   `set_tests_properties(proj_import_visibility_compile … proj_reserved_segment_compile ncc_no_o_output_cwd PROPERTIES TIMEOUT 180)`
   名单里也点着这个旧名（实测在 `:1038`）。漏掉这行的后果：改名后的条目**没有 TIMEOUT**，
   落回 CMake 默认的 1500 s 上限（这条是编译 fixture，红了会拖满 25 分钟而不是 3 分钟），
   同时名单里留着一个不存在的测试名，`set_tests_properties` 在 generate 阶段就会抱怨。
   `git add tests/CMakeLists.txt` 之前用
   `grep -n proj_reserved_segment tests/CMakeLists.txt` 收口，期望 **0 命中**（带文件名的
   两次命中说明只改了一半）。
   这条今天必然红：`RegisterUnit`（`ModuleRegistry.cpp:107-152`）**目前没有**任何重复路径检查
   （`grep` 实测：整个文件只有 `:138` 一处保留名调用），所以这是 Step 5 的**新行为**，
   要按 TDD 先加测试再实现。
3. **`tests/e2e/stdlib_reserved_names.n` 从 `compile_error` 翻成运行用例**
   （`tests/e2e/manifest.txt:775`）。文件内容注释写得很清楚，它钉的正是「`io`/`fs`/`math`
   三个内建命名空间名不许被用户占用」。保留名门一删，这段代码就是合法的普通声明：
   `int io(int x)` 的表键是 `stdlib_reserved_names.io`，与 `io.print` 不同名不冲突。
   期望值改成实测的退出码（`math = math + io(1)` ⇒ `return math` ＝ 2），
   **先跑一遍拿实际 rc 再写进 manifest**，不要照抄这里的推算。
   `manifest.txt:776` 的 `stdlib_array_arg compile_error` 与保留名无关，不动。
4. **删的是一条规则，不是一句话**（轮 6 实测，原稿的清单只到「发射那两条句子的 `if`」为止）：
   `grep -rn "is reserved for a library namespace" src` ＝ **2 命中**（`DuplicateFieldChecker.hpp:154`、
   `StatementResolverDecls.cpp:47`），但这两句各自嵌在一个**带调用者的检查函数**里，只删句子会把
   空转的判定留在树上：
   - `StatementResolverDecls.cpp:42-50` 是整个 `CheckLocalNameReserved`，删函数本体之外还有
     **5 处引用**必须同删：声明 `StatementResolver.h:48` ＋ 调用点
     `StatementResolverDecls.cpp:261`（局部 decl）、`StatementResolverFlow.cpp:81`（for-init）、
     `:264`（foreach 变量）、`StatementResolverSwitchTry.cpp:248`（catch 变量）。
     轮 6 实测：`grep -rn "CheckLocalNameReserved" src` ＝ **6 命中**（1 声明＋1 定义＋4 调用），
     一条都不许留——留着就是编译不过，删调用而留函数就是死代码。
   - `DuplicateFieldChecker.hpp:140-156` 要整块删：注释 `:140-146`、`fieldReg`／`ownerIsLibrary`
     两行 `:147-149`、`if` `:150-156`。**注意 `:147-149` 只服务这条保留名判定**（实测：
     `ownerIsLibrary` 在该函数后续不再被读，`sPrevName` 那一段与它无关），所以它们跟着一起消失；
     只删 `if` 而留下 `IsLibraryModule(OwnerOf(...))` 的取值，会让下一个读这段的人以为还有东西依赖它。
   - **不属于本步的一处**：`DuplicateFieldChecker.hpp:280` 的
     `m_Accessor.m_Env.IsLibraryNamespace(sName)` 是**类型别名**名占用检查
     （`:273-275` 的注释里那句「reserved stdlib namespaces」是旧措辞）。新模型里包名仍然来自路径，
     别名与包同名照样会让 `io.print` 这类限定解析产生歧义，所以**判定保留**，本步不动；
     它由 **Task 7 Step 2** 的 `IsLibraryNamespace`→`IsLibraryPackage` 改名带过去，
     注释里那句措辞按 Task 5 Step 6b 的收口 grep 一起清。


`git add` 要能看见第 2 条：Task 6 原稿只列了 `src/compiler tests/test_vm tests/test_compiler`，
**`tests/CMakeLists.txt` 与 `tests/e2e/` 不在内**，会把改名留在树里。

- [ ] **Step 6: 诊断文案的归属，加上本任务造出的三处文档失真**

**本步不改 `ModuleNotFoundText`**（`ModuleRegistry.cpp:100-105`）。轮 6 判定：原稿在这里写的
「加上找的是源码还是二进制」是一次**没有设计依据的文案扩张**——`docs/dev/phase5_design.md` 里
`grep -n "not found"` 只有 `:329` 那条 `readStruct` 的 `type not found`，与这条句子无关；
而改它要连带翻 4 处公开文字（见下面第 2 条），换来的信息「找的是源码还是 `.nmod`」在阶段 5
结束时**仍然不是用户能据以行动的**（带点名的预编译包搜索是阶段 6 的交付）。句子保持原样，
本步只做核对。

1. **两条金样本各自守一个发射器**（轮 6 实测：不是重复条目，别把它们并成一条）：
   - `tests/e2e/manifest.txt:1011` `import_dotted_singlefile`
     ＝逐字 `Module 'utils.helper' not found. Check the project Sources list or -I import path.`
     ⇒ **gate 分支**：`ApplyNonWildcardImport` 走到最后一行 `ModuleRegistryGate.cpp:100`
     （点分名、既没编进本次构建、也不是索引里的库）。
   - `tests/e2e/manifest.txt:1012` `import_not_found`
     ＝同形句、名字 `nosuch` ⇒ **loader 分支**：`ModuleRegistryGate.cpp:90-96` 把单段名录进
     `externalOut`，`LoadExternalModule` 找不到 `.nmod` 时在 `ModuleBuilderImports.cpp:197`
     发同一句。
   Step 3 删掉 `DiscoverLibraryUnits` 的 `continue` 之后，`utils.helper` 仍然「没有源码、也没有
   对应 `.nmod`」，`nosuch` 仍然只有单段那条路，所以**两条都必须逐字继续绿**。它们的含义在
   Step 3 前后变了：`:1011` 从「点分名语法上不可能解析」变成「搜索过而没有」，而串不变——
   这正是本任务需要的证据，别把它当成没动的行删掉。
2. **公开文字里逐字引用这句的 4 行**（en/zh 成对）跟着核对，串同样不变：
   `docs/user_manual/en/language-spec/declarations.md:65`、`docs/user_manual/zh/language-spec/declarations.md:58`、
   `docs/user_manual/en/language-spec/common-errors.md:60`、`docs/user_manual/zh/language-spec/common-errors.md:56`。
   **规则（本计划统一口径）**：文档里**逐字引用诊断句**的行，归改那句话的任务；
   **叙述性**的 `namespace` 用词归 Task 8。Task 8 的门只有 docs pytest，它不核对「文档说的规则
   与编译器当前行为是否一致」，所以Step 3/Step 5 当场造成的失真必须在本任务修，不能留给 Task 8。
3. **本任务真正要改口的三处文档**（轮 6 新增，原稿完全没有这一面，en/zh 成对）：
   - 点分导入规则：`docs/user_manual/en/language-spec/declarations.md:57-58`「Single-file mode (no
     `.nproj`) supports single-segment imports only — built-ins and external `.nmod`;
     **dotted paths cannot resolve**」＋ `docs/user_manual/zh/language-spec/declarations.md:49-51` 同形句。
     Step 3 之后点分名会去库根找 `<root>/a/b/c.n`，这句话把新行为写成不可能。改的时候要**说准
     剩下的限制**：带点名的预编译包（`.nmod`）仍然只按单段主干搜索（阶段 6）。
   - 保留名规则：`docs/user_manual/en/language-spec/declarations.md:56`「Project path segments may not
     collide with `io`/`math`/`fs` (compile error)」＋ `docs/user_manual/zh/language-spec/declarations.md:49`
     「项目路径段不得与 `io`/`math`/`fs` 撞名（编译错误）」——Step 5 退役那张表，这条规则不
     存在了，换成「同一次构建内包名重复 ⇒ 指名两个来源路径」的新规则（与 Step 5 第 2 条的
     `duplicate_package_path_compile` 用同一句措辞）。
   - 诊断块里的死句：`docs/user_manual/en/language-spec/declarations.md:67`／
     `docs/user_manual/zh/language-spec/declarations.md:60` 的
     `Module path segment 'io' collides with a built-in namespace.` 删掉（句子随表消失，留着
     就是卖一条已死的规则）；同块 `Module 'utils.helper' is not imported. Add 'import
     utils.helper;' (or 'import utils.*;') at the top of this file.`（en `:63`／zh `:56`）**保持**——
     它由 `ExprResolverStdLib.cpp:99` 那条硬编码 `Module '%s' …` 拼出，本阶段不动它的名词。
     块里 `Namespace 'io' is not imported. …`（en `:64`／zh `:57`）已由 **Task 5 Step 6b** 的
     清单认领，本步不重复改。

D5 的 native 诊断保持 Task 4 Step 10 的写法（`VmBackend.cpp:133-136` 调用侧）。

**保留名文案跟着表一起消失（轮 5 补：Task 5 Step 6b 的表把这 3 条明确留给本任务）**：
Step 5 退役保留名表之后，这三条句子连同它们的发射点一起删——
`src/compiler/builder/ModuleRegistry.cpp:142`（`Module path segment '<s>' collides with a
built-in namespace.`）、`src/compiler/builder/DuplicateFieldChecker.hpp:154` 与
`src/compiler/builder/StatementResolverDecls.cpp:47`（同一句
`The name "<n>" is reserved for a library namespace.`）。
**跟着改的门（轮 5 实测，不必再自己数）**：`grep -rn "reserved for a library namespace" tests`
＝ **0 命中**，`grep -rn "collides with a built-in" tests` ＝ **2 命中** ——
`tests/CMakeLists.txt:1020` 的 `PASS_REGULAR_EXPRESSION "collides with a built-in namespace"`
（换成新的撞名诊断正则，否则那条 ctest 在文案改后**静默不匹配**。轮 7 注名：Step 5 已经把这个
条目改名成 `duplicate_package_path_compile`，`add_test` 与 `:1038` 的 TIMEOUT 名单两处都要跟着走，
这里再按新名改期望串——三处不同名会各自留下半条死引用）＋
`tests/test_compiler/test_module_import.cpp:470-471` 的期望串
`"Module path segment 'io' collides with a built-in "`。
`tests/e2e/manifest.txt:775` 的 `stdlib_reserved_names compile_error` 按 Step 5 的说明翻转成新期望。
**公开文字侧**（轮 6 实测：`grep -rn "collides with a built-in" docs` ＝ **2 命中**，
`grep -rn "reserved for a library namespace" docs` ＝ **0 命中**）就是上面第 3 条已经认领的
`docs/user_manual/en|zh/language-spec/declarations.md:67/:60`，两边不是重复记账：测试侧拔的是**断言**，
文档侧删的是**死句引用**。

- [ ] **Step 7: 全量＋e2e 语料＋文档门（本步不碰文法，冲突数不变）**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
D:/dev/miniconda3/python.exe tests/e2e/run_e2e_tests.py \
  build-dev/src/tools/ncc/Release/ncc.exe build-dev/src/tools/nvm/Release/nvm.exe \
  | tee /tmp/e2e.log; grep -c '^SKIP' /tmp/e2e.log      # 期望：0
PYTHONPATH=tools/nlang-docs/src NLANG_NCC=$PWD/build-dev/tests/Release/ncc.exe \
NLANG_NVM=$PWD/build-dev/tests/Release/nvm.exe \
D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests -q
```
期望 **64/64**（门限见 Global Constraints）＋ e2e **977 passed / 6 failed** ＋ docs **67 passed**（974 基线＋Task 1 的三条
`qhead_*`；六条既有失败逐字见 Task 1 Step 2 的块）。**本任务对 `manifest.txt` 只做一件事**＝翻
`stdlib_reserved_names`（`:775`）的期望。原稿写的「改 `:1010`～`:1012` 三条金样本的串」有两处错
（轮 6 订正）：`:1010` 是 **Task 5 Step 6b** 改的（`Namespace 'io'` → `Package 'io'`），不归本任务；
`:1011`/`:1012` 按 Step 6 第 1 条**逐字不动、且必须仍然绿**——它们是这条文案两个发射器的守卫，
把它当「要改的串」就会顺手改掉一句本任务并不改的诊断。**docs 门本步必跑**（轮 7 把理由换准）：
跑它是因为 Step 6 第 3 条**删掉了文档里的整句**，要确认没有把链接或站点构建打坏；
但**不要指望门替我们盯双语正文的成对性**：Step 6 动的是两份**已存在**的页面、不动 nav，
而 `test_tree_parity` 的三条断言只比页面清单／pending 名单／两份 nav 的路径集合，
**不看正文行**（详见 Task 7 Step 4 的注）⇒ 删 en 忘了删 zh 是静默的，靠清单逐条勾。
**轮 5 补**：原稿这里只有 `ctest` 一行，既没有构建命令（Step 3～6 改的
是 `src/compiler`，不重编就跑门＝跑旧二进制），也没有 `run_e2e_tests.py`——而本任务**确实**翻动
`tests/e2e/`：`manifest.txt:775` 的 `stdlib_reserved_names compile_error` 行随保留名表退役而换
期望串，`proj_reserved_segment/` 目录改名。这两处只被 runner 执行（`manifest.txt` 不在 ctest 里，
见 Task 4 Step 11 的轮 4 实测）。按 Task 4 Step 11 立下的规则——凡改动 `tests/e2e/**` 的任务
（Task 1、Task 4、Task 5、Task 6）门里都带这条命令——补齐。

- [ ] **Step 8: Commit**

```bash
git add src/compiler tests/test_vm tests/test_compiler \
        tests/CMakeLists.txt tests/e2e \
        docs/user_manual/en/language-spec/declarations.md docs/user_manual/zh/language-spec/declarations.md
git commit -m "feat(compiler): resolve dotted library imports and derive packages from the matched root"
```
`git status --porcelain` 在提交后必须只剩基线行 `?? main.n`（Global Constraints 记的门禁基线，
轮 7 订正：原稿写「必须为空」，而这道门在本工作树里永远非空，永远不通过的门没人当门看）
——Step 5 的 fixture 目录改名／`manifest.txt`
的翻转都在 `tests/e2e/` 里，漏掉就是把删除留在树里。两条 `docs/.../declarations.md` 是轮 6 补的：
原稿的 `git add` 没有 `docs`，Step 6 第 3 条那三处失真（「点分路径无法解析」「段名不得与
`io`/`math`/`fs` 撞名」「`Module path segment 'io' collides …`」）会留在手册里，而 Task 8 的门
只看 en/zh 配对与链接，看不出「文档写的规则已经是假的」。

---

### Task 7: 连带面——langservice、nide、`.nproj`

**Files:**
- Modify: `src/langservice/SymbolIndex.cpp:82-119`（`NamespaceScope`，Step 2 改名后消失）、
  `:146-177`（`ConsumeIndexLine` 的硬门 `:154-155`）、`:64-80`（`BuildSymbol`）、
  `:234-246`（`LoadLibraryDir`）、`:248-282`（`Resolve`／`CompleteNamespace`／`Namespaces`）
  ＋`include/nlang/langservice/SymbolIndex.h:61/77/81/86`（公开面改名，Step 2）
  **注**：这些文件的大部分在 **Task 5 Step 3c** 已经改过；本任务的 Step 2 只做**改名**这一层，
  所以真正落在本提交的是 `.h` 的四个名字与三个调用点。
- Modify: `src/tools/nide/CodeEditor.cpp:242-283/285-307/380-394/396-422/424-435/437-445`
  （轮 3 勘误：**没有 `src/tools/nide/editor/` 这一级**，文件就是 `src/tools/nide/CodeEditor.cpp`；
  六个区间本身经实测是对的，`:432` 是 `CompleteNamespace` 的调用点）
- Modify（**轮 5 按实测补齐，轮 6 复数订正：测试面是 26 行／4 文件，不是 27**＝
  `test_dialogs.cpp` 9＋`test_mainwindow.cpp` 1＋`test_projectmodel.cpp` 15＋
  `test_solutiontreemodel.cpp` 1，与轮 5 自己列的逐条清单一致；原稿的 27 是笔误，
  照抄会让盘点的人多找一行。生产面 ＝ 20 行／6 文件，两条命令与逐条清单在 Step 4）：
  `src/tools/nide/ProjectModel.h:62/63/142`、
  `ProjectModel.cpp:35/36/37`、`ProjectModelXml.cpp:244/245/284/308`、
  `ProjectPropDialog.cpp:64/111/154/165`、`ProjectPropDialog.h:32`、
  `ui/ProjectPropDialog.ui:61/64/67/70/279`、`src/tools/ncc/ProjectFile.h:12/18`（注释），
  ＋测试 `tests/test_nide/{test_dialogs.cpp,test_mainwindow.cpp,test_projectmodel.cpp,test_solutiontreemodel.cpp}`
- Modify（**轮 6 新增：`namespace=` 字面属性面；轮 7 把命令定成带 `-I` 的形式
  `grep -rIn 'namespace=' docs examples src tests tools` ＝ 18 行／15 文件**；不加 `-I` 是
  20 行／17 文件，多的两条是 `__pycache__/*.pyc`，前两条 grep 看不见它，
  逐条处置在 Step 4 的第三条命令下面）：
  `docs/user_manual/en/cli-tools/ncc.md:60`、`docs/user_manual/zh/cli-tools/ncc.md:55`、
  `examples/hello_project/hello_project.nproj:2`、
  `tests/e2e/proj_*/{…}.nproj:2`（7 个目录）、`tests/test_ncc/test_projectfile.cpp:41`、
  `tools/nlang-docs/src/nlang_docs/snippets.py:181`（`_NPROJ` 模板）。
  这 12 行（docs 2＋examples 1＋e2e 7＋test_projectfile 1＋snippets 1）
  ＋上面测试组里的 `test_projectmodel.cpp` 4 行＋`test_solutiontreemodel.cpp` 1 行
  ＋`ProjectFile.h:12` 注释 ＝ 18（别把重复的四条再改一遍）。
  **18／15 是在 `d7ca710` 量的，本任务在 Task 6 之后跑，必须先重量**（轮 7 补，B7）：
  Task 6 Step 5 把 `tests/e2e/proj_reserved_segment/{目录, .nproj, ctest 条目}` 整体改名成
  重复包名 fixture，上面 7 个 e2e 目录里的第 5 条（实测 `tests/e2e/proj_reserved_segment/proj_reserved_segment.nproj:2`）
  到时候**路径与文件名都不一样**，行号也可能不再是 `:2`（新 `.nproj` 要列两个库根、两份
  `graphics.n`，Sources 段变长）。所以 Step 4 的第三条命令在落地时以**重跑结果**为准逐条勾，
  总数写成「以本任务开跑时的 `grep -rIn 'namespace=' docs examples src tests tools` 为准」，
  不要拿 18 当验收值；`proj_*/` 那一行改述成「7 个（Task 6 改名后仍是 7 个，名字换了）」。
  改名后的新 fixture 若仍写 `namespace=` 就还是 18／15；若不写，就是 **17 行／14 文件**
  （那个文件整个从命中集里消失）——少一行／一个文件不是漏改，别为凑数补回去。
- Modify: `src/compiler/BuildEnvironment.cpp:55-57`（`:55` 是 `IsLibraryNamespace` 签名行、
  `:57` 是 `HasNamespace` 调用点，Step 2 两个名字都要改）＋
  `IsLibraryNamespace` 的声明（`include/nlang/compiler/BuildEnvironment.h:106`）与**剩余**调用点
  （**轮 6 实测**：今天 `grep -rn "IsLibraryNamespace" src include` ＝ 1 声明＋1 定义＋**7 调用**，
  但本任务在 Task 6 之后跑，`DuplicateFieldChecker.hpp:151` 与 `StatementResolverDecls.cpp:45`
  那两处已随保留名判定整块删除 ⇒ 本任务实际改 **5 处调用**：
  `DuplicateFieldChecker.hpp:280`、`ExprResolverStdLib.cpp:68`、`ExprResolverStdLib.cpp:87`、
  `ExprResolverTypes.cpp:372`（**不是 `:364`**，那是注释行）、`ModuleBuilderImports.cpp:87`。
  逐条说明见 Step 2）
- Modify: `tests/test_langservice/test_symbol_index.cpp:99/125`、`tests/test_nide/test_mainwindow.cpp:611`、`tests/test_nide/test_searchpath_integration.cpp:79`
- Test: 上述

- [ ] **Step 1: `SymbolIndex` 不在本任务（轮 2 去重）**

`langservice::SymbolIndex` 的名字来源已经在 **Task 5 Step 3c** 与去壳同一次提交里改完
（理由：`kHead` 正则一失效索引就空，`test_library_index.cpp` 与 `BuildEnvironment.cpp:33-34`
会在 Task 5 当场红，拆到本任务必然过不了门）。本任务**不重复**那条改动，只做它剩下的连带面：
nide 的补全假设、`.nproj` 的 `namespace=` 属性，以及下面 Step 2 的 API 改名。
所以本任务没有「先加失败测试」这一步——它的失败侧在 Task 5 已经吃过一次，
强行再写一条红测试就是空转（轮 2：原稿的 Step 1/2/3 与 Task 5 Step 3c 是同一段活）。

- [ ] **Step 2: `ns` 词汇一次性改到底（原稿留给本步「决定」，现在定）**

`SymbolIndex` 的公开面按已定的命名模型改名，不留两套叫法：
`CompleteNamespace`→`CompletePackage`（`SymbolIndex.h:77`，定义 `SymbolIndex.cpp:257`）、
`Namespaces()`→`Packages()`（`:81`）、`HasNamespace`→`HasPackage`（`:86`，定义 `:277`），
`NamespaceScope` 类名（`SymbolIndex.cpp:82-119`）随 Step 3c 的正则删除一起消失，
`SymbolInfo::ns` 字段→`pkg`。

**`IsLibraryNamespace` 的改名面（轮 6 实测：原稿的「生产调用点只有三处」不成立，而且它把
已被 Task 6 删掉的调用点也算进来了）**：

```bash
grep -rn "IsLibraryNamespace\|HasNamespace" src include | cat
```
- `HasNamespace`：声明 `include/nlang/langservice/SymbolIndex.h:86`、定义
  `src/langservice/SymbolIndex.cpp:277`、**唯一生产调用点 `src/compiler/BuildEnvironment.cpp:57`**
  （原稿这个行号是对的）。
- `IsLibraryNamespace`：声明 `include/nlang/compiler/BuildEnvironment.h:106`（**不是原稿写的
  `ModuleBuilder.h:162`**，那个文件里没有这个声明）、定义 `src/compiler/BuildEnvironment.cpp:55`
  （**不是 `:56`**：实测 `:56` 是函数体的 `{`，签名在 `:55`，
  `return m_upLibraryIndex && m_upLibraryIndex->HasNamespace(name);` 在 `:57`）。
  调用点在 **Task 6 Step 5 之后**只剩 **5 处**：`DuplicateFieldChecker.hpp:280`（别名名占用检查，
  判定保留，见 Task 6 Step 5 第 4 条）、`ExprResolverStdLib.cpp:68`、`ExprResolverStdLib.cpp:87`
  （Step 6b 的 `pKind` 那一条）、`ExprResolverTypes.cpp:372`（**不是原稿写的 `:364`**，`:363-364`
  是注释；这一处就在 `RejectUnimportedQualifiedType` 里）、`ModuleBuilderImports.cpp:87`
  （通配符谓词的 lambda 体）。今天 grep 到的另外两处——`DuplicateFieldChecker.hpp:151` 与
  `StatementResolverDecls.cpp:45`——是**保留名判定**，Task 6 Step 5 第 4 条已经连函数带调用点
  整块删除；本步若还「照着今天的 grep」去改名它们，就是改两行已经不存在的代码。
  ⇒ 本步实际动作＝**7 行**（1 声明＋1 定义＋5 调用）＋ `SymbolIndex` 那三条名字。
- nide 侧调用点：`src/tools/nide/CodeEditor.cpp:432`（`CompleteNamespace`，原稿正确）。
测试侧调用点用 `grep -rn "Namespace" tests/test_langservice tests/test_compiler` 现场数，
不照抄清单。

- [ ] **Step 3: nide 补全假设校准**：`CodeEditor.cpp:242-283` 的排除注释
> 「library calls are exactly `ns.name`」按新规则重写；点链补全的候选来源改为包名表。
> **IDE 可见行为必须由用户交互验收**——完成后停下来请用户在 nide 里验
> `io.` 补全、`vendor.graphics.` 补全、F12 跳转，再算本步通过。

- [ ] **Step 4: `.nproj` 的 `namespace=` 属性删除**（从没被 ncc 读过；留着就是
> 第三个名字来源）。`ProjectFile.h:12/18` 的自述注释同步删。
>
> **轮 4 实测＋轮 5 复核（原稿只列了两个生产文件，删到一半必红；轮 4 记的「38 行／10 文件」
> 与「12 处测试点」是两条**互相不自洽**的数：它列的测试点是 8＋1＋2＝**11**，而且 pattern 里
> 混了泛用的 `Namespace` 词元。轮 5 换成下面这两条**只打真属性面**的命令，逐条重数过）：**
>
> ```bash
> grep -rn 'edtNamespace\|namespace_()\|setNamespace\|m_namespace\|"namespace"\|命名空间' \
>   src/tools/nide --include=*.cpp --include=*.h --include=*.ui | sort -t: -k1,1 -k2,2n   #生产 20 行／6 文件
> grep -rn 'edtNamespace\|namespace_()\|setNamespace\|namespace=' tests/test_nide \
>   --include=*.cpp | sort -t: -k1,1 -k2,2n                                               #测试 26 行／4 文件
> grep -rIn 'namespace=' docs examples src tests tools                          #字面属性面 18 行／15 文件
> ```
> 第三条**必须带 `-I`**（轮 7 实测）：不带是 **20 行／17 文件**，多出来的是
> `tools/nlang-docs/src/nlang_docs/__pycache__/snippets.cpython-313.pyc` 之类编译缓存里的
> 「Binary file … matches」——它们既不该改、也不该数。
> - 生产（20）：`ProjectModel.h:62/63/142`（访问器对＋成员）、`ProjectModel.cpp:35/36/37`
>   （`setNamespace` 本体含发射 dataChanged）、`ProjectModelXml.cpp:244/245/284/308`（写两端＋读两端）、
>   `ProjectPropDialog.cpp:64/111/154/165`（创建回填／编辑回填／新建清空／载入填值四条）、
>   `ProjectPropDialog.h:32`（注释里的 `edtNamespace`）、
>   **`src/tools/nide/ui/ProjectPropDialog.ui:61/64/67/70/279` 五处**（`:61` 的 toolTip
>   `模块命名空间`、`:64` 的可见标签 `命名空间(&amp;S):`、`:67` buddy、`:70` widget、`:279` tabstop。
>   轮 4 记的四处**漏了 `:61` 的 tooltip**，那也是一句用户可见中文）；
>   `CodeEditor.{h,cpp}` 的命中属于 Step 2 的 `IsLibraryNamespace`→`IsLibraryPackage` 改名，
>   不是这条属性。
> - 测试（**必须同提交**，否则 `dialogs_tests`／`projectmodel_tests`／`mainwindow_tests`／
>   `solutiontreemodel_tests` 全红，**26 行／4 文件**，轮 7 实测按文件分解
>   9＋1＋15＋1＝26；轮 5 此处的「27 行」是加法 slip）：
>   `test_dialogs.cpp:160/171/245/260/287/294/301/311/318`（9，轮 4 漏 `:287`）、
>   `test_mainwindow.cpp:166`（1）、
>   `test_projectmodel.cpp:85/93/97/310/327/399/421/487/492/606/623/657/666/685/1093`（**15**，
>   轮 4 只列了 `85/97` 两条）、`test_solutiontreemodel.cpp:30`（1，轮 4 整条没提——
>   那是 `.nproj` XML 模板串，删属性后模板里留着 `namespace="ns"` 就是让一个没人读的属性
>   继续出现在测试语料里）。
>   这些用例钉的是「对话框把名字回填进模型」，删字段时把断言换成模型剩下的那个字段（`name`），
>   **别整条删掉**——删掉就把对话框回填行为的钉子一起拔了。`:1093` 那条尤其注意：它用
>   `setNamespace("unsaved")` 当**弄脏模型的副作用**，删掉之后要换一个仍存在的 setter
>   （`setName` 之外还有 `setOutputDir`），否则「未保存」这半条断言会静默失效。
>
> **第三条命令量的是前两条看不见的面**（轮 6 实测；前两条只扫 `src/tools/nide` 与
> `tests/test_nide` 的 `*.cpp`，而这个属性还出现在**磁盘上的 `.nproj` 文件、文档样例、
> 文档门的模板**里，那些地方没有 C++ 标识符可打）。**18 行／15 文件**（带 `-I`；不带是
> 20 行／17 文件，见上面的注），逐条处置：
> - `docs/user_manual/en/cli-tools/ncc.md:60` ＋ `docs/user_manual/zh/cli-tools/ncc.md:55` ——给用户看的 `.nproj`
>   样例。**归本步**（不是 Task 8）：按本计划的归属规则「逐字引用被删对象的行，跟着删它的
>   那一步走」。同提交的理由要说准（轮 7 更正）：**不是**「否则文档门红」——
>   `test_tree_parity.py` 三条断言（`:58/:70/:79`）比的是**页面清单、pending 名单、两份 nav 的
>   路径集合**，`test_two_column_css_is_mirrored` 比的是 CSS 字节，**没有一条看正文行**；
>   所以「en 删了 zh 没删」在这一面是**静默的**。它归本步、与 en 同提交，靠的是
>   本仓库的成对文档约定（docs 页必须双语同批改），不是靠门兜。
> - `examples/hello_project/hello_project.nproj:2` ——**随包发的**样例（`verify_package.py:118-140`
>   的 `scan_public_text` 会走 `examples` 目录），Task 5 Step 5 已经把它推到本步。
>   同上一条，它也是**活输入**：`tests/CMakeLists.txt:920/:930/:938` 三条用例直接
>   `ncc -p` 这份文件（`:930` 是 `ncc_p_conflict`，故意把 `.nproj` 和裸源混喂的负例），
>   删属性后这三条照常绿。
> - `tests/e2e/proj_{bare_cross_dir,dup_import,import_visibility,not_imported,reserved_segment,same_dir,wildcard_exact_union}/*.nproj:2`
>   ＝ **7 行／7 文件**。这 7 份**是活输入**：`tests/CMakeLists.txt` 给每个目录各挂一条
>   `proj_*_compile`（`ncc build -p …/….nproj`）＋`proj_*_run`（例如 `:966-973`），
>   跑完照常绿——正因为 ncc 从不读这个属性，删它才会「过门但没人发现」。
>   而 `grep -rn nproj tests/e2e/manifest.txt` 只有 `:1009` 一句说明，**没有任何一条金样本
>   钉 `.nproj` 的字节**，所以门既不会替我们留它也不会替我们清它 ⇒ 必须跟属性同批手动删，
>   否则会永久留在仓库里。
> - `tests/test_ncc/test_projectfile.cpp:41` ——只是**输入**字面量，全文件对该属性**零断言**
>   （实测该文件的 `namespace` 命中只有 `:13` 的 `namespace fs = std::filesystem;` 和这一行）。
>   按 `test_solutiontreemodel.cpp:30` 同一条理由删：没人读的属性不该继续出现在测试语料里。
> - `tests/test_nide/test_projectmodel.cpp:487/492/623/657` ＋ `test_solutiontreemodel.cpp:30`
>   ——已列在上面「测试（26 行）」组内；其中 **`:623` 是写侧断言**
>   （`QVERIFY(content.contains("<Project name=\"Hello\" namespace=\"hello\" outputDir=…>"))`），
>   它和 `ProjectModelXml.cpp:244/245` 的写端**必须同一提交**，隔一步就是红的。
> - `tools/nlang-docs/src/nlang_docs/snippets.py:181` 的 `_NPROJ` 模板 ——**文档门自己的写端**
>   （多文件 snippet 会合成一份 `.nproj`）。实测 `grep -rn '_NPROJ\|namespace' tools/nlang-docs/tests/`
>   ＝ **0 命中**，改它不动任何断言、也不会被门拦住；但不改就是「文档正文里已经没有这个属性，
>   而门生成的工程文件还在写它」——一条只在门内部自洽、对外已经假的规则。
>   ⇒ 与 `docs/*/cli-tools/ncc.md` 同批改；`git add` 的 `tools`／`docs`／`examples` 见 Step 6。
>
> 删 `.ui` 里的 widget 属**用户可见**变化：uic 重生成后停下来请用户在 nide 里开
> 项目属性对话框确认（与 Step 3 的补全一起验收一次即可）。

- [ ] **Step 5: 全量＋部署检查＋文档门**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
PYTHONPATH=tools/nlang-docs/src NLANG_NCC=$PWD/build-dev/tests/Release/ncc.exe \
NLANG_NVM=$PWD/build-dev/tests/Release/nvm.exe \
D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests -q
```
期望 **64/64**（门限见 Global Constraints：Task 4 起 64），其中 `mainwindow_tests` 与 `nide_deploy_check` 必须串行（并行跑会假失败）；
文档门期望 **67 passed**（轮 6 补：本任务改了 `snippets.py` 的 `_NPROJ` 模板，
`tools/nlang-docs/` 是被门看的目录，改它不跑门＝本计划的收口门漏了一条）。
`NLANG_NCC`／`NLANG_NVM` 不给会有 5 条**静默 skip**，67 变 62 也不算红——所以这两条
环境变量是这一步的一部分，不是可选项。守卫用的 Python 是
`D:/dev/miniconda3/envs/py313/python.exe`（裸 `python` 是 WindowsApps 占位符，
报 `Permission denied` 不是测试失败）。

- [ ] **Step 6: Commit**

```bash
git add src/langservice src/tools/nide src/tools/ncc src/compiler include/nlang tests docs examples tools
git commit -m "refactor(tools): index and complete by path-derived packages, drop .nproj namespace"
```
**轮 4 订正：原稿的 `git add` 少了 `src/compiler` 与 `include/nlang`**——Step 2 改的
`BuildEnvironment.cpp:55-57`／`IsLibraryNamespace` 声明处（`include/nlang/compiler/…`）／
`SymbolIndex.h`（`include/nlang/langservice/…`）全在这两条里，
漏了就把跨任务的改动留在树里（`git status --porcelain` 除基线行 `?? main.n` 外必须空，
这条是本计划的收口门——轮 7 订正：写「必须空」的门在本工作树永远红，永远红的门会被 eyeball 掉）。
**`src/tools/ncc` 是轮 6 补的**：Step 4 要删 `.nproj` 的 `namespace=` 属性，它的自述注释在
`src/tools/ncc/ProjectFile.h:12`／`:18`（实测该文件另有 `:8` `namespace nlang {` 与 `:36`
`} //namespace nlang` 两行是 C++ 语法，**不动**），而这三条目录里 `src/tools/ncc` 原本不在
`git add` 清单上——属性删了、注释还写着「namespace/intermediateDir are IDE-facing」，
`git status` 也会当场不空。
**`docs`／`examples`／`tools` 也是轮 6 补的**，对应 Step 4 第三条命令的三个面：
`.nproj` 文档样例（`docs/user_manual/{en,zh}/cli-tools/ncc.md`）、随包发的样例工程
（`examples/hello_project/`）、文档门自己的 `.nproj` 写端（`tools/nlang-docs/…/snippets.py:181`）。
原来这三条目录都不在清单上，属性删完 `git status --porcelain` 会在基线行 `?? main.n` 之外
多出 ` M`／`??` 行——而收口门正是它（Global Constraints 已把基线定成那一行，
轮 7：原稿写「必然不空」，在有基线行的树里这句话本身永远成立，抓不到漏加）。

---

### Task 8: 文档页＋ CHANGELOG（en/zh 成对，双导航）

**Files:**
- Create: `docs/user_manual/en/language-spec/packages.md`、`docs/user_manual/zh/language-spec/packages.md`
  （轮 3 实测：`docs/` 今天**只有** `cli-tools/`、`getting-started/`、`language-spec/`、
  `vm-architecture/` 四个层级，**没有 `guide/`**；包名属于语言语义 ⇒ 放 `language-spec/`）
- Modify: `docs/user_manual/en/**` **10 个文件／29 行**、`docs/user_manual/zh/**` **4 个文件／5 行**的 `namespace`
  叙述（**这是 `d7ca710` 上的小写 grep 基线，不是本任务开工时的数**——Step 1 说明为什么必须在
  本任务重取；原稿的「14 个英文文件／34 处、9 个中文文件／28 处」
  是把 en＋zh 合起来的输出误读成分语言数字）。逐文件分布：
  en `vm-architecture/library-mechanism.md` 8、`language-spec/declarations.md` 7、
  `language-spec/standard-library.md` 5、`vm-architecture/module-serialization.md` 2、
  `getting-started/faq.md` 2、`language-spec/naming-convention.md` 1、
  `language-spec/common-errors.md` 1、`getting-started/running.md` 1、`cli-tools/ncc.md` 1、
  **`getting-started/function-features.md` 1**（轮 4 补：原稿列了 9 个文件却说「10 个文件／29 行」，
  少的就是这条；补完合计 29 ✓）；
  zh `cli-tools/ncc.md` 1、`getting-started/faq.md` 1、**`language-spec/declarations.md` 2**、
  `vm-architecture/library-mechanism.md` 1（轮 4 订正：原稿写「各 1」，实测 `declarations.md`
  是 2 行；四文件合计 5 ✓）
- Modify: `mkdocs.en.yml:34-48`／`mkdocs.zh.yml:34-48` 两套 nav 各加一行
  （**轮 6 实测订正：`mkdocs.base.yml` 里没有 `nav:`**——`grep -n "nav:" mkdocs.*.yml` 只命中
  en／zh 两个文件，base 是 `INHERIT` 的共享层（theme/markdown_extensions/hooks），本阶段
  不动它；原稿写「Modify: `mkdocs.base.yml`（＋ en／zh）两套 nav」会把人引去改一个没有 nav 的文件）
- Modify: `CHANGELOG.md`

- [ ] **Step 1: 盘点（轮 6 改命令：必须大小写不敏感，且基线要在本任务开工时重取）**

```bash
#分语言数，别把两条命令的输出合起来当单语言数字（原稿就是这么读错的）
for d in en zh; do
  echo "== docs/$d =="
  grep -rni "namespace" docs/$d | cut -d: -f1 | sort | uniq -c | sort -rn
  grep -rni "namespace" docs/$d | wc -l
done
```
**为什么是 `-rni` 而不是 `-rn`**（轮 6 实测）：在 `d7ca710` 上小写 grep 给出 en 29／zh 5，
大小写不敏感给出 **en 33／zh 8**——差的 8 行（en 5＋zh 3）全是大写 `Namespace`，其中
6 行（en 3＋zh 3）是**逐字引用编译器整句**的 `Namespace 'io' is not imported. …`，已由
**Task 5 Step 6b** 认领并改成 `Package 'io'`；剩下 2 行是本任务的叙述面
（`docs/user_manual/en/getting-started/running.md:57`「**Namespace**: the module namespace.」、
`docs/user_manual/en/vm-architecture/library-mechanism.md:64`「**Namespace-qualified calls compile as
ordinary calls.**」）。用原稿那条小写命令盘点，这 2 行会**静默漏网**，而它们正是本阶段
概念清零的对象。

**基线不是 29/5**（轮 6）：那两个数在 `d7ca710` 上量，而 Task 5 Step 6b（删 `declarations.md`
的「已知限制」段、改 `function-features.md`/`common-errors.md` 的引用句）与本任务 Step 6
第 3 条（改 `declarations.md:56-58`／`:67` 三条失真）都已经动过这两棵树。
⇒ **本任务开工第一步重跑上面两条命令**，拿当时的数字作基线写进提交说明，并逐条给出
「删／改写为目录分组」的处置；对不上 29/5 **不是**漏改的证据。
新页内容除了 Step 2 列的那几条，必须写明两件本阶段落地的用户可见变化：
① D13 的拼写变化（`b main` → `b main.main`、回溯里的 `at main.main`）；
② D9 的**实测文案**——限定类型上的泛型实参今天得到的是
`syntax error`（`Task 2 Step 7b` 实测），指名文案是阶段 7 的缺口，不要在文档里写成已经给。


- [ ] **Step 2: 写新页**（中英成对）：包名＝路径、限定符原文、`.nmod` v1.13 与限定表键、
> `readStruct` 字面量需唯一可见、`native` 限单段包、`b <name>` 断点须写限定名、
> 对象流里的类型名现为限定名（跨程序语义变更）。

- [ ] **Step 3: 两套 nav 各加一行**，然后跑配对门：

```bash
PYTHONPATH=tools/nlang-docs/src NLANG_NCC=$PWD/build-dev/tests/Release/ncc.exe \
NLANG_NVM=$PWD/build-dev/tests/Release/nvm.exe \
D:/dev/miniconda3/envs/py313/python.exe -m pytest tools/nlang-docs/tests -q
```
期望 **67 passed**（含 `test_tree_parity.py` 的 en/zh 配对与 linkcheck）。

- [ ] **Step 4: 零旧引擎痕迹自查**

```bash
grep -rniE "<旧引擎名>" docs/ stdlib/ src/compiler/grammar/ | grep -v CHANGELOG
```
公开文字必须为空命中。

- [ ] **Step 5: Commit**

```bash
git add docs CHANGELOG.md
git commit -m "docs: package names come from paths; type identity is qualified (en+zh)"
```

---

### Task 9: 审核闭环（阶段 5 的出口条件）

- [ ] **Step 1: 4b-2 round 2**——派新评审 subagent，范围 `1bce551..HEAD`，
> 逐条处置第 1 轮遗留（`docs/dev/phase4b2_audit_notes.md` 的 Important 2/3/4 与四条 Minor）。
> 评审者必须自己重跑：串行 ctest、`test_library_source`、docs gate。
- [ ] **Step 2: 阶段 5 自己的新鲜评审轮**——新评审者、范围 `d7ca710..HEAD`，
> 不带上一轮结论。
- [ ] **Step 3: 循环**：每轮按 `receiving-code-review` 先对树核实（复现／反驳）再处置；
> 有新增 Critical/Important 就修，然后**再派一个全新评审者**。直到某轮报告
> 「无新增 Critical 且无新增 Important」才算阶段结束（用户规则，见
> [[feedback-phase-audit-loop]]）。评审笔记写进 `docs/dev/phase5_audit_notes.md`。
- [ ] **Step 4: 更新状态记忆**：`project-phase4-library-mechanism.md` 记阶段 5 已闭合、
> 下一步是阶段 6（`docs/dev/phases_567_design.md` §2 的 8 项交付）。

## Self-Review（写完计划后自查）

1. **Spec 覆盖**（轮 1/2 修订后的真实映射，原稿这段有假勾选）：R1→Task 3（缝）＋ Task 5
   Step 3b（容器退役）＋ Task 6；R2→Task 4（含 Step 8b 的调试面 D13）；R3→Task 6；
   R4→Task 1（形状＋检查）＋ Task 2（`':'`／`as` 扩点）＋ **Task 2 Step 7b（D9，原稿缺）** ＋
   Task 5；R5→Task 5/7/8。D1→Task 3 ＋ **Task 5 Step 3/3b**（轮 4 订正：原稿指「Task 5 Step 4」，
   那一步是 bison 冲突计数，不产生任何判定行为）；D2→Task 4 Step 8；D3→Task 6；
   D4→Task 4 Step 6 ＋ Task 6 Step 5；D5→Task 4 Step 10；D6→Task 1；D7→Task 1/2；
   D8→Task 3；D9→Task 2 Step 7b；D10→Task 4 Step 9；D11→Task 4 Step 3/4；D12＝提交顺序；
   **D13（调试面显示名）是新加的，需要用户点头**（Task 4 Step 8b），它的**门**在轮 6 才补上：
   D13 唯一的用户可见输入面是 `ndisasm -func <表键>`，而那个工具原本只被 `version_ndisasm`
   跑过一次 ⇒ Task 4 **Step 8c** 新增本计划唯一一条 ctest 条目。
   **R1 还剩两处没排进任何任务**（轮 1 判定，待用户决定是否本阶段做）：
   `Access(SnIdentifierExpr)` 的类型候选不做 owner 过滤（`ExprResolverTypes.cpp:410-450` 的
   `bareFuncFilter` 只管函数，`:432` 的 `FindFieldInAncestor` 对类型是裸 first-wins）、
   `ResolveClassBases`（`StatementResolverTypes.cpp:27-56`）的非限定分支仍走裸名。
   后果：导入了含 `Point` 的库时，本 TU 的裸 `Point q;` 会绑到库的那个。**设计稿
   `phase5_design.md:84-92` 说「共 5 处」，实测其中 3 处（`CompiledInFunctions`／
   `FindModuleType`／流式字面量）已被任务覆盖，剩下 2 处需要单独决策**；
   且设计稿给的行号 `ExprResolverTypes.cpp:425-433` 指的是函数过滤器，不是类型分支。
2. **占位符**（轮 4 重扫，原稿这一段的自我描述已经过期）：
   - Task 4 Step 5 是一张站点表——每行给出确切的旧表达式与新表达式，不是「加适当处理」，
     这条仍然成立且是**有意**的（35 处站点写进散文里不可读，表本身没有留白）。
   - Task 6 Step 1 原稿只给三条签名＋一句「每条给完整源」——**那是占位符，已经补全**
     （轮 4：五条完整用例，含设计 §4 与 §9／D5 两条原本没排进来的；轮 5 再补第 6 条
     `TestSameLastSegmentPackagesCoexist`＝设计 §2，并把 Task 2 凑不出前提的设计 §11
     后半搬到 `(4c)`。同轮删掉 `(1)` 里那条 `namespace wrong` 外壳——Task 6 在 Task 5 之后，
     那个源已经写不出来）。
   - Task 4 Step 1b 的 `(3)` 原本也只有签名，**已补全**（`(3a)`～`(3d)`）。
   - **执行者仍要自己写源文件字面量的地方只剩一处，且理由成立**：Task 6 Step 1 的库根布局
     依赖 Step 3 的匹配根实现，用例里的相对目录名以现场实现为准；这条要在开工第一步
     对着 Step 3 的代码确认，不是照抄本文件。
3. **类型一致性**：`PackageOf`／`QualifiedName`（Task 3 声明，Task 4 使用，
   Task 4 Step 9 用 `Registry().QualifiedName`）；`KeyOf`（Task 4 Step 4 定义，Step 5
   站点表使用）；`MarkMalformed`／`IsMalformed`（Task 1 定义——**没有 `SetSegments`**，
   Task 1 Step 5 明确写了不加它，`AppendSegment` 够用，轮 4 把这里的自相矛盾删掉）；
   `TypeName` 范畴（Task 2）；`FindLibrarySourceFile` 返回 `pair`（Task 6 Step 3 定义，
   Step 3 的 `DiscoverLibraryUnits` 使用）。
4. **测试面自洽（轮 4 补）**：Task 1／Task 4 Step 1b／Task 5 Step 1b／Task 6 Step 1 现在
   逐条对得上设计矩阵 §1～§15；**§13 的三条链负例走的是 e2e 子进程门（进程内是 UB，
   见 Task 1 Step 1 的实测块），不在 ctest 里**，所以每条碰 `tests/e2e/**` 的任务
   （1、4、5、6）的门里都必须带 `run_e2e_tests.py` 那条命令，光跑 ctest 不算过门。
   Task 4 Step 1b 的四条文案断言吃它；**Task 1 的 Step 8 终门原本也没带它**，轮 5 已补。
   矩阵里 §2（同末段两包共存）与 §11 的后半（包在、类型不在）在轮 5 之前**没有任务**，
   现在分别由 Task 6 Step 1 `(6)` 与 `(4c)` 拥有。
   矩阵里唯一没有行为钉子的是 §8 的**运行面**（用户 `Object` 的方法调用）——
   那是编译器侧按裸名短路到内置单例的 5 个点，本阶段未排期，已报用户裁决。
5. **文案面（轮 5 新增的一整层，原本全阶段没人管）**：删掉 `namespace` 关键字之后，
   产品代码里仍有 **8 条用户可见句子**带「namespace」这个词，另有 3 条跟着保留名表一起在
   Task 6 消失。处置排期见 **Task 5 Step 6b**（含 `tests/e2e/manifest.txt:1010` 与
   `tests/test_compiler/test_module_import.cpp:1427-1460` 两条金样本／整句断言）与
   **Task 6 Step 6**（保留名文案＋`tests/CMakeLists.txt:1020` 的正则）。
   这条是「公开文字零旧概念」约束的最后一处缺口。
6. **门的形状（轮 6 新增的一整层：少报的门比没有门更坏）**：这一轮修的四条门，共同毛病是
   **在不该跳的时候静默跳过**，所以每条都补了「为什么这命令会骗人」——
   ① `grep -rn … \| grep -v '^\s*//'` 是空转过滤器（7 进 7 出），换成两条各答一个问题的
   命令并给实测基线；② 大小写敏感的 `grep namespace docs` 少 6 行，换 `-rni` 且规定
   开工时重取基线；③ 从构建日志 grep bison 冲突数，在不改文法的重跑里是**空输出**，
   四处统一换成直接 `win_bison`（Task 5 Step 4 本来就这么写，其余三处现在一致了）；
   ④ `namespace=` 属性面在两条 C++ 标识符 grep 里**根本不存在**，Task 7 Step 4 补第三条
   命令；⑤ ndisasm 全生命周期只跑过 `--version`，Task 4 Step 8c 补唯一一条新 ctest 条目。
   连带后果：**ctest 门限不再是常数**，Global Constraints 写成曲线（Task 1～3＝63/63，
   Task 4 起＝64/64），Task 4/5/6/7 的四处期望值与 Task 6 Step 5 的「改名复用、总数不变」
   已逐处对齐；Task 7 Step 5 补文档门（那一步改了 `snippets.py`，不跑 pytest 就是漏门）。
   公开文字面的归属规则同步收敛为一条：**逐字引用被删对象的行，跟着删它的那一步走**
   （`.nproj` 文档样例 ⇒ Task 7 Step 4；诊断句 ⇒ Task 5 Step 6b／Task 6 Step 6；
   纯叙述 ⇒ Task 8），Task 8 的盘点因此不再重复这些行。

---

## 计划审核记录（实施前置门，用户规则：审核没有新问题才开始动手）

### 轮 1（三名新审阅者，视角互不重叠：事实核对／需求覆盖／提交链可行性）

**接受的（都在代码里复核过才改）**：

| 来源 | 结论 | 落到 |
|---|---|---|
| C-C1 | 库成员住在 `namespace <path>` 壳里，root 单扫必丢全部库符号 → Task 3 的门红 | Task 3 改纯加法；容器退役搬进 Task 5 Step 3b |
| C-C2 | `SymbolIndex.cpp:98-99` 正则只认 `namespace`，去壳即索引空、`test_library_index.cpp:47` 的 38 红 | Task 5 Step 3c（从 Task 7 并过来） |
| B-C1 | R1 五处 owner 盲查找只覆盖了 3 处 | 记进 Self-Review 第 1 条，两处留给用户裁决 |
| B-C2 | D9 完全没实现步骤，Self-Review 的勾选是假的 | Task 2 Step 7b |
| B-C3 | 对象流 `VmExecutorSer.h:204-206 ↔ :290-304` 写读同源，计划一个字没提 | Task 4 Step 5 表尾两行 |
| A/C | 站点表不闭合（14＋11 处缺） | Task 4 Step 5 表 ＋ `grep \| wc -l ＝ 35` 的闭合命令（轮 3 订正：1745 行下面那条审计记录里写的 38 是**把 `include/nlang/vm` 也加进去**的另一种数法；计划前置事实的口径是 `src/vm`＝35，两处必须同一说法。另：`test_library_index.cpp:47` 的 `size() == 38` 是**索引条目数**，与这两个数无关，别混） |
| C-C4＋自查 | 表键＝显示名，`main`→`main.main` 打穿回溯／断点／nide | Task 4 Step 8b（新增 D13，待用户确认） |
| C-C6＋自查 | 函数侧查重会误杀合法重载（`SnMisc.cpp:88-115`） | Task 4 Step 6 改成只查类型表 |
| A-T4-1 | `VmExecutor.h:402-404` 的 `FindEnum("Object")` 原稿没列，限定后静默失效 | **轮 2 作废**：该文件该区间没有这段代码，`FindEnum` 全仓库零命中；真实位置是 `VmExecutorOpsObjects.cpp:321-330`＋`RegisterClass.cpp:173-179`＋`BuiltinNames.h:28-31`，已写进 Task 4 Step 5 表 |
| A/C | 负控在提交前跑会冲掉同文件改动 | Task 4 Step 12/13 调换顺序 |
| C-I1 | 保留名表退役会拔掉一条注册 ctest（`proj_reserved_segment_compile`）却没说总数变 | Task 6 Step 5：**条目改名复用**（`duplicate_package_path_compile`），总数保持当时的门限值（轮 6 注：Task 4 Step 8c 之后门限是 **64**，保持的是「本任务不增不减」这条，见 Global Constraints 的门限曲线）；同一步把 `stdlib_reserved_names` 从 compile_error 翻成运行用例 |
| A/B/C | 路径错：`TestNatives.h` 在 `src/vm/`、`MachineFrontEnd.cpp` 在 `src/tools/ndb/`、fixture 是 `mylib/mylib.n` | Task 4/5/7 的 Files |
| A-T2/T4 | `ClassInheritOpt` 的 union 槽（`nlang.y:311`）与 `SnAsExpr` ctor 形参不改就编不过 | Task 2 Step 3 |
| A-T4-1 | `Register.cpp:16-43` 是匿名命名空间，成员定义写进去非法 | Task 4 Step 4 的位置说明 |
| B-M2/I3 | `cases[]` 死代码、新用例没说要注册进 `main()` → 门空转 | Task 1 Step 1、Task 3 Step 1 |
| A-前置事实 | 格式 12 的硬编码不止三处（两篇 module-serialization ＋ `ModuleSaver.cpp:39`） | 前置事实 |

**驳回的（审阅者读数与代码不符，留档免得重跑一轮）**：

- 「`CollectQualifiedSegments` 今天已经返回 bool」→ `nlang.y:160` 实测 `static void`。
- 「`FindModuleType` 没有 `isLibrary` 分支、`:359-397` 不存在」→ `:378` 就是。
- 「`CompiledModuleNodeBuilder.hpp:425` 不存在」→ 存在，且 `if (cf.name == "main")` 逐字对。
- 「`TestNatives.h` 注册的是内建 `sys.print`/`io.readLine`，`native` 不出现」→ `src/vm/TestNatives.h:61-64` 是四个裸名 native。
- 「`:178-179` 是通配符 `continue`」→ 通配符在 `:171-172`，`:178-179` 才是带点名。
- 「编译器从不写 `isNative`，那条分支是死的」→ `VmBackend.cpp:133-136 → :182-190` 写。
- 「删 `namespace` 净增 1 条 sr（期望 15）」→ 按本步的确切删除形状实测 **14 sr**（`b.y` 与删完后的 `NamespaceMember` 逐行同形），计划原稿的 16＝14＋2 成立。
- 「`stdlib/*.n` 被 gitignore，要 `git add -f`」→ `git ls-files -- stdlib` ＝ 3，正常入库。

### 轮 2（三名新审阅者读修订稿：A＝代码事实／B＝计划自洽／C＝测试风险）

**接受的（每条都先落到代码或实跑再改）**：

| 来源 | 结论 | 落到 |
|---|---|---|
| A | 格式 12 的硬编码不止前置事实列的三处：`docs/user_manual/en\|zh/vm-architecture/module-serialization.md:15/:13` ＋ `ModuleSaver.cpp:39` 的「single source」注释 | 前置事实第 70 行 |
| A | Task 2 Step 3 的编辑清单只列 `nlang.y:258-262`，漏了 `:311` 的 `%type <v_pNameExpr> ClassInheritOpt` 与 `SnExpressions.h:660` 的 `SnAsExpr` **ctor 形参**——两处不改就编不过 | Task 2 Step 3「两处必改的槽类型」 |
| A | Task 2 Step 4 点错了对象：基类槽是 `SnClassDecl::SuperName()`，**今天就已经是 `SnFieldExpr*`**（`SnMisc.h:267`，ctor `:261`）；`BaseName()` 是泛型擦除键（`const std::string&`）、`SuperClass()` 是解析后的 `SnClassDecl*`，都不该改 | Task 2 Step 4 措辞按实测重写 |
| A | 站点表两行自相矛盾：`EmitExprNew.cpp:55-56` 前置事实列「必改」、表里写「不变」；`EmitStmtSwitchTry.cpp:428` 整行缺失 | Task 4 Step 5 表（含泛型只有内建三种 ⇒ `KeyOf` 一条规则覆盖，无需分支） |
| A | 轮 1 那行 `FindEnum`／`InitStructHeap`／`GetOrMatchStruct`／`g_lastCaller` 是幻觉符号；真实的内建 `Object` 裸名判定是 `RegisterClass.cpp:173-179`＋`VmExecutorOpsObjects.cpp:321-330`＋`BuiltinNames.h:28-31` | Task 4 Step 5 表尾三行＋轮 1 该行标记作废 |
| A | 「删除 `FindFunction`/`FindStruct`/`FindClass`」不可执行：调用点数量级不允许（`FindStruct` 12／`FindClass` 18／by-name finder 1 生产＋10 测试），且 `Find*(` 的清单本身不闭合——表键**写入端**与两处**手工扫表**都不在里面 | Task 4 Step 3 改「不删 finder，逐点改缝」＋前置事实第 97 行＋Step 5 表写入端两行 |
| A | 版本门原稿只写地板：本任务自己的 ctest 门会红（ceiling 是另一个 `if`、`test_debugger.cpp` 三处 `0x0C` 硬编码、今天没有「太新」的负例） | Task 4 Step 8 的四点版本护栏块 |
| A | 原稿写「owner＝入口 TU」，但本仓库没有「入口 TU」这个概念；设计 §4 的「多个 TU 各有 `main()`」没排进任何任务 | Task 4 Step 7 的判定规则＋≥2 分支用例 |
| B | `ModuleBuilderImports.cpp` 里根本没有 `reserved` 字样（原稿让人去那儿拔钉子）；撞名诊断的真钉子是三处 | Task 6 Step 5 的三处清单 |
| B | Task 5 Step 2 预测「正例必失败」被一条**通过的**基线测试反驳：`test_library_source.cpp:402` 已经是正例 | Task 5 Step 2 改写＋「正例不用新写」 |
| B | Task 7 与 Task 5 Step 3c 重复同一处 `SymbolIndex` 改动，拆两个提交必过一个门 | Task 7 Steps 1-3 折叠（轮 3 定名） |
| C | Task 4 的测试面缺四条：路径优先于壳名、同包重名类型拒绝（含跨包对照）、流面字面量编译期解析、入口往返 | Task 4 Step 1b |
| C | 负控（关掉缝即失败）在提交前跑会冲掉同文件改动——与轮 1 的 C 视角同一发现，两次独立指出 | Task 4 Step 12/13 顺序 |

**驳回的（读数与代码不符）**：

- 「`MergeFrom` 在 `src/compiler/SnProgram.cpp:245`」→ 本仓库没有 `SnProgram.cpp`；定义在 `SnMisc.cpp:50`，调用点在 `ModuleBuilder.cpp:245`。
- 「`ModuleLoader` 的地板／天花板是同一个 `if`」→ 实测两个独立 `if`（`:69` 地板 `< 12`、`:71` 天花板 `> kCurrentMinorVer`）。
- 「`ndb` 有一层 request 协议要跟着改」→ `MachineFrontEnd.cpp:110`、`:195`、`.h:143` 里的 "requests" 全是注释措辞，不存在请求层；命令面在 `:135-160`。
- 「`stdlib/io.n` 的 `readLine` 在 `:5`」→ `wc -l` 实测 io 23／math 65／fs 26 行，行号一律以本仓库当前文件为准，别接受任何单点行号快照。
- 「`ModuleFunctions` 在 `:304`／`SuperName` 在 `SnMisc.h:239`」→ 真实位置 `ModuleRegistry.cpp:313`、`SnMisc.h:267`。
- 「`Register.cpp:56`／`:58-62` 是入表点」→ `:16` 开匿名 namespace、`:43` 闭合，`:47` 起是 `RegisterStructDecl`；class 与 struct 的名字写在两个地方（`RegisterClass.cpp:93-102` vs `Register.cpp` 的 struct 记录）。
- 「`SnQualifiedTypeExpr` 已经能表达 malformed」→ 该类在 `SnExpressions.h:568`，今天没有任何 bool/标志位。
- 「加一条『库里声明泛型 `alib.Vec<int>`』的测试」→ 泛型只有内建三种（`List`/`Dict`/`Func`，`ExprResolverTypes.cpp:88`），用户类型带实参是 D9 的拒绝面，这条测试今天和改完之后都过不了。

### 轮 3（后台新审阅者复测修订稿＋我自己跑 ncc/grep/wc 复数）

**接受的**：

| 来源 | 结论（验证方式） | 落到 |
|---|---|---|
| 复测 | `ExprResolverCast.cpp` 里没有 `as` 处理；真正的目标解析是 `Access(SnAsExpr&)`（`ExprResolverValues.cpp:115`，同文件另两个函数是 `LogArrayBindingReject:114`、`FixupParamTypesWithBindings:127`） | Task 2 Step 4 定位 |
| 复测 | `src/vm/CMakeLists.txt:48-50` 是源文件列表，include 块在 `:55-61`，没有 `PRIVATE include ${PROJECT_SOURCE_DIR}/src/compiler` 这一行 | Task 4 Files 勘误 |
| 复测 | `StatementResolverTypes.cpp` **零改动**：`Resolve()`（`ExprResolver.h:744-751`）是 `sn.Accept(m_Visitor); return sn.IsResolved();` 的虚分派，限定节点进槽后自动走对分支 | Task 2 Step 4／提交面说明（`:616`） |
| 复测 | `test_module_import.cpp` 是 QtTest（moc 发现槽），原稿让执行者往 `main()` 里注册用例 → 门空转 | Task 3 Step 1 测试样板改真 `QVERIFY/QCOMPARE` |
| 复测 | `ModuleLoader.cpp:124-136` 不是「导入模块 stub」路径（那是模块名读取）；导入侧的 `entryPoint` 读出来即弃 | Task 4 Step 7 勘误 |
| 复测 | native fixture 是 `tests/fixtures/native/mylib/mylib.n`，两个文件各在自己同名子目录下，不在 `native/` 根下 | Task 5 Files |
| 复测 | `ImportedNamespaces()` 全仓库零命中；`ModuleBuilder.cpp:129-155` 是 `RegisterUnits()`，`MergeTransUnits` 在 `:233-258` | Task 3/4 引用订正 |
| 实测 | **D9 的今天的真实行为**：跑 `build-dev/tests/Release/ncc.exe`，`alib.Vec<int> v;` 得到裸 `syntax error`，永远进不了 `CollectQualifiedSegments` ⇒ 指名文案推迟到阶段 7，本阶段只保留「不误接受」的负例 | Task 2 Step 7b＋Task 1 Case 2 |
| 实测 | 深链失败面：`alib.twice(1).Box v;` → `Compiler internal error: bad allocation`（原稿的方括号形状是普通语法错误，测不到链）；`compileLog` 必须自己吞异常，否则进程内 `bad_alloc` 打死整个测试二进制 | Task 1 Case 2／Step 1 helper |
| 实测 | 金样本闭合：`tests/e2e/manifest.txt` 只有 5 行的期望串嵌了函数名；`-func` 没有 e2e 用例；未命中路径今天全仓零测试 | Task 4 Step 11／Step 9 |
| 实测 | e2e fixture 同步面不是「14 个文件」；`test_library_index.cpp` 的 38 是**索引条目数**（实测 decl：io 5／math 25／fs 8）⇒ 保持 38，只改措辞，数字变了先查再改 | Task 5 Step 3c／Task 6 Files |
| 实测 | 站点数口径统一：`src/vm` ＝ 35，把 `include/nlang/vm` 一起数才是 38；两处必须同一说法 | 前置事实＋轮 1 该行订正 |
| 实测 | docs 连带面：en 10 文件／29 行（原稿漏 `cli-tools/ncc.md`），zh 4 文件／5 行；新页面落在 `docs/user_manual/en\|zh/language-spec/packages.md`（没有 `guide/` 这一层）；计数按语言分别跑 | Task 8 |
| 复测 | `SymbolIndex` 的去壳连带改名当场定死：`CompleteNamespace`→`CompletePackage`（h:77/cpp:257）、`Namespaces()`→`Packages()`（h:81）、`HasNamespace`→`HasPackage`（h:86/cpp:277）、`SymbolInfo::ns`→`pkg`、`IsLibraryNamespace`→`IsLibraryPackage`；生产调用点 `BuildEnvironment.cpp:57`＋`src/tools/nide/CodeEditor.cpp:432`（不是 `nide/editor/`） | Task 5 Step 3c／Task 7 |
| 复测 | Task 6 Step 5 的 ctest 条目改名复用，总数保持 **63**；`stdlib_reserved_names`（`manifest.txt:775`）从 compile_error 翻成运行用例 | Task 6 Step 5 |

**驳回的**：

- 「`EmitExprInitList.cpp:305-307` 用 `pElemType` 查表，是表键写入点」→ 那三行是 `classDecl.BaseName().empty() ? classDecl.Name() : BaseName()`（`EmitInitListClassForm`），该处没有 `pElemType`。
- 「Task 4 的 CMake 改动要给 `src/vm` 加 `src/compiler` 的 PRIVATE include」→ 该 include 块（`:55-61`）今天就在，不需要动。
- 「把 `FindEnum("Object")` 那行按 `VmExecutor.h:402-404` 改掉」→ 该区间是 NativeHost 回调；`FindEnum` 全仓库零命中（已并进轮 2 的作废行）。
- ~~「`src/vm` 的 `src/compiler` PRIVATE include 今天就在，Task 4 不用改 CMake」~~
  **这条驳回是轮 3 我自己写错的**（轮 4 的 A 抓到）：`src/vm/CMakeLists.txt:55-61` 的
  PRIVATE 段只有 `:60` 一行 `${CMAKE_CURRENT_SOURCE_DIR}`，没有任何 `src/compiler` 条目，
  Task 4 Step 3 确实要加。轮 4 记录里已把这条按实测翻案，别再按「不用动」执行。

### 轮 4（三名新审阅者读轮 3 修订稿＋我自己跑 ncc/nvm/ndisasm/grep 复每一笔）

视角同轮 2（A＝代码事实、B＝计划自洽与交叉引用、C＝测试面与门）。**这一轮的多数结论是
我用真二进制实测出来的**，不是再读一遍代码——凡「审阅者说 X，我看 X」的一律进「驳回」或
「实测」栏，方法写清楚。

**接受的（每条后面的括号＝我的复核办法）**：

| 来源 | 结论 | 落到 |
|---|---|---|
| A | `src/vm/VmExecutorOpsCalls.cpp` 不在 `backend/` 下；`nlang.l` 的 `"namespace"` 在 `:174` 不是 `:169`；漂移守卫在 `tools/nlang-docs/tests/test_highlight.py`（`tests/test_docs/` 不存在）；`ModuleNotFoundText` 声明在 `ModuleRegistry.h:33`（`:38` 是 `IsReservedLibraryName`） | Task 4 Files／Task 5 Files／Task 6 Files／sed 全局订正 |
| A | `MergeTransUnits`（定义 `:233`）由 **`ResolveAll()`（`:85`）在 `:89`** 调用；`DiscoverLibraryUnits():71`／`RegisterUnits():74` 在 `PrepareUnits()`（`:59`）里，`Build()`（`:98`）只是两者的调用者——原稿把三步串成 `Build()` 的一条顺序是错的（复核：`grep -n 'ModuleBuilder::' src/compiler/ModuleBuilder.cpp`） | Task 6 Files |
| A | 表键写入点少一行：**`VmBackend.cpp:418` `desc.name = name`**（LocalDescriptor 局部名，保持裸名但要写明）⇒ 全表 18 行；闭合命令 `grep -rn '\.name = \|\.name ==\|name != ' src/vm include/nlang/vm src/tools/ndisasm/main.cpp \| nl` | 前置事实＋Task 4 Step 5 |
| A | `test_debugger.cpp` 的名字键是 **14 处**不是 8；版本 pin 是 **三处**（`:254/:300/:348`，降级字节 0x09/0x0A/0x0B，改 0x0C→0x0D 要三处一起）；`DebugSessionController.cpp:139-165` 是 `AddFunctionBreakpoint`（**全仓没有 `AddLineBreakpoint`**）；tool main 里没有任何 native 字面量，四个裸名只在 `src/vm/TestNatives.h:61-64` | Task 4 Files／Step 8b |
| A | **轮 3 我自己那条驳回是假的**：`src/vm/CMakeLists.txt:55-61` 的 PRIVATE 段只有 `${CMAKE_CURRENT_SOURCE_DIR}`，没有 `src/compiler` 条目，Task 4 Step 3 确实要加 | Task 4 Files＋轮 3 记录划掉（见上） |
| B | Task 4 Step 1b 用了两个**不存在**的 helper（`compileDirOnly(dir,&pMod)`、三参 `compileRun(dir,cap,&pMod)`）；真实面只有 `compileDir(dir)→bool`（`:68-84`，入口硬编码 `main.n`）与 `compileRun(dir,cap)→int`（`:90-100`），模块按 `ModuleLoader::Load` **按值**读回（`:94-95`） | Task 4 Step 1b 重写（含 `compileDir` 加带默认值的入口文件名参数） |
| B | Task 4 Step 1b 的 `(3)` 只有签名＝占位符；Task 6 Step 1 的三条也只有签名＝占位符（原稿 Self-Review 还说「唯一需要补源的地方是 Task 6」，两处矛盾） | 两条都补全；Self-Review 第 2 条重写 |
| B | **设计矩阵有两条没排进任何任务**：§3（项目侧 `utils/helper.n`＋`core/helper.n` 同末段）没人写；Task 3 Step 2 的两条去向也指错（§2 要到 **Task 6 Step 1** 才凑得出带点 import，「路径压壳名」其实**已经**写在 Task 4 Step 1b `(1)`） | Task 5 Step 1b（新增）＋Task 3 Step 2 重写 |
| B | D5／§9「多段包内 `native` 是诊断」的**测试**没排：诊断在 Task 4 落地，但 Task 4 的单段场景凑不出多段包，全阶段零钉子；§4 的三段路径＋父段不给子段也没排 | Task 6 Step 1 `(4)`/`(5)` |
| B | Task 7 Step 4 只列两个生产文件，实测面是 **38 行／10 文件**：`ui/ProjectPropDialog.ui:64/67/70/279`（含可见标签）＋`ProjectModel.{h,cpp}`＋`ProjectPropDialog.{h,cpp}`＋**12 处测试点**（`test_dialogs.cpp` 8＋`test_mainwindow.cpp:166`＋`test_projectmodel.cpp:85/97`）；Task 7 的 `git add` 少了 `src/compiler` 与 `include/nlang` | Task 7 Step 4／Step 6 |
| B | Task 8 的 en 枚举列了 9 个文件却说「10 文件／29 行」，少 `getting-started/function-features.md`；zh 说「各 1」，实测 `declarations.md` 是 2 行（复核：分语言 `grep -rn namespace docs/user_manual/en \| uniq -c`） | Task 8 Files |
| C | **`tests/e2e/manifest.txt` 不在 ctest 里**（`grep -n manifest tests/CMakeLists.txt` ＝ 0，ctest 只有 `e2e_compile`＝`tests/CMakeLists.txt:914`，只编 `examples/hello.n`）⇒ 碰 `tests/e2e/**` 的任务（1/4/5/6）光跑 ctest **不算过门** | Task 1 Step 2／Task 4 Step 11＋每条门 |
| C | Task 4 Step 5 表里 `EmitExprInitList.cpp:305-307` 那行的代码抄错了（真实是 `classDecl.BaseName().empty() ? classDecl.Name() : BaseName()` ＋ `FindClass(className)`，没有 `pElemType`） | 该行改判 `KeyOf(classDecl)` |
| C | Task 3 Step 1 的 `(c)` 共用了一份**没有 import** 的 gate 工程，而库单元只有被 import 才进 AST root（`PrepareUnits():59`→`DiscoverLibraryUnits():71`），那条 `findMember(..., NK_Namespace, "io")` 会因无关原因红 | Task 3 Step 1 `(c)` 用自己的 `ioOpts` |
| C | Self-Review 的类型一致性栏写了 `SetSegments`，而 Task 1 Step 5 明确决定**不加**它 | Self-Review 第 3 条 |

**我自己测出来的四笔（不在任何审阅者报告里，都改了计划／设计）**：

1. **Task 1 的两条链负例是活的 UB，不是可捕获的异常**。同一份 `alib.n`＋
   `alib.twice(1).Box v;` 跑 5 次：run1/2/4/5 `Compiler internal error: bad allocation`、
   run3 `Error: Module 'alib. 4' is not imported`（脏名字）；`q[0].x v;`（q 是 int 变量）
   ⇒ **0xC0000005**，shell 报 Segmentation fault、rc=139。所以负例挪去 e2e 子进程门
   （崩了只砸一条 manifest 条目），`compileLog` 的 try/catch **作为既有 `compileDir:80-81`
   那层保护的改写版保留**，但它不再为任何断言服务——Step 1b 的四条文案断言走的是正常
   日志通道。矩阵 §13 跟着改。
2. **用户 `Object` 今天坏在磁盘层之上**：根上写 `class Object { int marker; }`，
   `ndisasm` 实测类表 `[0] Object (fields=0)`（内置）＋ `[11] Object (fields=1)`（用户）
   **两个同名键**，而 `new Object()` 编的是 `class=0`，运行 `x.marker = 5` ⇒
   `NLang VM: struct field store out of bounds`；换成方法 `who()` 则编译期就报
   `The function "who" does not exist or is not accessible`。根因在编译器侧按**裸名**
   短路到内建单例的 5 个点（`ExprResolverNew.cpp:120`、`ExprResolverTypes.cpp:393`、
   `ExprResolverValues.cpp:46`、`ExprResolverStdLib.cpp:59`、`DuplicateFieldChecker.hpp:279`），
   **本阶段未排期**，要用户裁决。矩阵 §8 的钉子因此只钉表键、不钉运行面
   （Task 4 Step 1b `(5)` 的注释写明了这条边界）。
3. **`readStruct("S")` 对库里的类型今天是直接失败**（ncc 实跑：`ReadStruct type not found: S.`）——
   `ExprResolverMemberBuiltins.cpp:277-282` 的收集面是调用者的 **AST 作用域链**，而 `import`
   从不往链上加节点（D2）。所以 Step 9 不是「换判据」，是**换信息源**（注册表的可见模块集）。
   同一条 Step 9 的歧义文案让用户写 `'pkg.S'`，那就必须让**带点字面量真的能用**，
   否则建议在骗人 ⇒ 新增 `(3d)` 正例。
4. **三段类型名在声明位不需要新语法**：`A.B.S v;` 实测解析通过，报的是 resolver 的
   `Module 'A.B' is not imported. Add 'import A.B;' ... before using type 'S'.`
   不是 `syntax error`。Task 6 Step 1 `(4)` 因此确认「只等带点 import」，不加语法工作。

**驳回的（每条都实测或逐行核对过，写下来是为了防止下一轮又被提上来）**：

- 「`SnProgram.cpp:245` 是 `MergeFrom` 的调用点」→ **本仓库没有 `SnProgram.cpp`**；
  定义在 `SnMisc.cpp:50`（`SnNamespace::MergeFrom`），调用点 `ModuleBuilder.cpp:245`。
- 「`ModuleLoader.cpp` 的地板与天花板在同一条 `if`」→ `:69` 与 `:71` 是**两条独立 `if`**，
  天花板由 `:59` 的 `kCurrentMinorVer` 驱动。
- 「ndb 断点走的是 request protocol，`b <name>` 不用改」→ `src/tools/ndb/DebugSession.cpp:236`
  ＋`DebugSessionController.cpp:139-165` 是裸名比较（`:152`），要改。
- 「stdlib 三个文件的 `namespace` 行在 `:5` 之外还有别的」→ 实测外壳行就是
  `io.n:5`／`math.n:5`／`fs.n:5` 各一行，声明数 5／25／8＝38 与 `test_library_index.cpp:47` 一致。
- 「`ModuleFunctions` 在 `:304`」→ `:313`。「`SuperName()` 在 `SnMisc.h:239`」→ `:267`
  （ctor `:261`，`BaseName()`／`SuperClass()` 在 `:268-269`，本阶段不许动）。
- 「`Register.cpp:56/:58-62` 是类型表键写入点」→ `:16` 开匿名 namespace、`:43` 收，
  `:47` 是 `cs.name = sn.Name()`（结构体键）；类键在 `RegisterClass.cpp:93-102`，
  函数键在 `Register.cpp:272`。
- 「给库里的泛型类型补一条用例」→ 泛型只有内建 `List`/`Dict`/`Func`（D9），
  用户类型带实参今天走到的是裸 `syntax error`，本阶段只钉「不误接受」。
- 「收口 grep 里加一条 `grep -v 'namespace fs'` 更干净」→ **这条过滤器会把
  `stdlib/fs.n:5` 的 `namespace fs {` 一起吞掉**（`fs` 是 C++ 别名噪声，也是库文件名）。
  要用 `namespace fs =`（带等号），跑完先看行数再看内容。

### 轮 5（三名新审阅者读轮 4 修订稿＋我自己跑 ncc/nvm/ndisasm＋grep 逐条核实）

**接受的（每条都在树里或实跑里复核过，写明复核方法）**：

| # | 发现 | 我怎么核实的 | 落点 |
| --- | --- | --- | --- |
| A | Task 6 Step 1 `(1)` 的 fixture 写 `namespace wrong {…}`，而 Task 5 已删该产生式 ⇒ 本任务在 Task 5 之后，这条源**根本编不出来**；且 D8「路径压壳名」已由 Task 4 Step 1b `(1)` 拥有 | 读 Task 5 Step 3（删 `Namespace` 产生式）＋设计 §4-13（`namespace x {}` 必须是语法错误）；两处的任务顺序是 4→5→6 | Task 6 Step 1 `(1)` 改成裸体库源，D8 声明归 Task 4 |
| B | **设计 §2（同末段两包共存）全阶段没人钉**：Task 3 Step 2 只留了一句「要到 Task 6 Step 1 才凑得出」的指针，Task 6 Step 1 里没有这条 | 逐条比对 `phase5_design.md:299-345` 的 15 格与计划里的用例；§2 无对应函数 | Task 6 Step 1 新增 `(6)` `TestSameLastSegmentPackagesCoexist` |
| C | Task 4 Step 11 金样本表把 `manifest.txt:1021` 推成 `main.main`，与同表 `:1026/:1029` 用的「单文件⇒包名＝stem」规则自相矛盾 | `sed -n '1018,1033p' tests/e2e/manifest.txt` ＋ `ls tests/e2e/dbg_*`：用例文件是 `dbg_break_continue.n`，单文件 | 该行改 `dbg_break_continue.main` |
| D | Task 5／Task 6 的门里没有 `run_e2e_tests.py`，但两任务都动 `tests/e2e/**`，违反 Task 4 Step 11 自己立的规则 | Task 5 的理由是实测的：`stdlib_sqrt_canary.n`（`manifest.txt:774`）跑 `import math;`＋`math.sqrt(4.0)` 整条管道，正是 Step 5 去壳的对象；`grep -rn namespace tests/e2e --include='*.n'` 的 4 个命中全在注释里（轮 7 纠正：**7 命中／4 文件**，
「4」是文件数；另见本步坑① 的归属纠正） | Task 5 Step 7／Task 6 Step 7 补命令（Task 6 顺带补上原本缺失的 `cmake --build`） |
| E | Task 1 的 §13 只有两种形状，缺 `a[0].x v;` | 实跑 `ncc build` 同一形状 3 次：脏段名／`bad allocation`／`bad allocation`（轮 4 记的 AV 本轮未复现，但三种出路已足够排除进程内断言） | Task 1 Step 1 新增 `tests/e2e/qhead_index.n`＋manifest 第三行；Step 8 终门与 Step 9 的 `git add` 同步 |
| F | Task 2 Step 5 只说「末尾追加」，账本头一句会一直写 14，而 Step 6 的门期望 16 | `grep -n conflicts temp/probe/base.output`＝148/185/191/254/290＝14；`temp/probe/d.output`＝72/73/145/181/187/250/286＝16，新增正是 72/73；`'^State 72$'` 段体是 `'.' shift` vs `rule 130 (TypeName)` | Task 2 Step 5 改成「总数 14→16、三族→四族＋新族条目」，并写明状态号会随语法改动整体平移 |
| G | Task 4 的 `KeyOf` 导出面自相矛盾（Interfaces 说不导出，Step 4 说把 `QualifiedName` 声明进头，Step 5 的表从约十个文件调 `KeyOf`） | `grep -c VmBackend:: src/vm/backend/{EmitExprNew,RegisterClass,EmitCall}.cpp`＝5/10/13，`VmBackend.h` 的 `public:` 在 `:61` | Interfaces 与 Step 4 统一：`VmBackend::KeyOf` 声明进 `VmBackend.h` 公开段、定义在 `Register.cpp`；二参自由函数 `QualifiedName` 留文件级 |
| H | Task 5 的去壳清单漏了 Task 1 的 `qhead_call`/`qhead_deep` 两份 `alib.n` 与 Task 4 Step 1b 的壳 fixture；Task 3 Step 1 `(c)` 的 `NK_Namespace "io"` 下钻也会随去壳红 | 后果实测推理：Task 5 之后 `namespace alib {` 写不出来 ⇒ 那两条 e2e 的 stderr 变 `syntax error` ⇒ 金样本红；`(c)` 的断言对象（壳节点）消失 | Task 5 Files 补 ①②③ 三条，各给处置（`(1)` 改裸体源、`(c)` 改 root 直取、禁止双形状回落 helper） |
| I | 格式号 12 的**文档**面（`module-serialization.md` 每语言 3 处）没排进任务，而 Task 8 的盘点门是 `grep namespace`，扫不到不含该词的行 | `grep -n "minorVer\|< 12\|v1.12" docs/{en,zh}/vm-architecture/module-serialization.md`＝en `:15/:23/:34`、zh `:13/:21/:28` 共 6 处 | Task 4 Step 8 新增第 5 条，六处逐条列明 |
| J | Task 4 Step 8 第 4 条的 `TestModuleVersionCeiling` 没注册进步手 `main()` | `test_debugger.cpp` 无 `static void Test*`；用例是 `test_*` 自由函数，`main()` 从 `:2213` 逐条调用，`:2222-2225` 正是四条版本拒绝用例 | 改名 `test_loader_accepts_ceiling_and_floor()` 并写明注册行位置＋照 `:240-263` 的补丁写法 |
| K | Task 2 Step 7 钉的「Type 'C' is not a member of namespace 'a.b'」在该前提下**发射不到**，且「parse 得下来」对 `:` 位不成立 | 实跑四份最小源：`class D : a.b.C`／`class D : a.b` ＝ `syntax error`；`a.b.C v;` ＝ `Module 'a.b' is not imported.`；`x as a.b.C` ＝ `Cannot resolve the field: a.`。`ExprResolverTypes.cpp:344-358` 的两个分支核实：未导入 ⇒ `:373-377`，已导入缺类型 ⇒ `:355` | Step 7 整段重写（含新用例正文）；「缺类型」那半搬到 Task 6 Step 1 `(4c)`，并规定只钉 `is not a member of` 前缀 |
| L | **用户可见文案里的 `namespace` 一词全阶段没人清**（删语法之后仍卖旧概念，违反公开文字约束） | `grep -rn '"[^"]*namespace[^"]*"' src/…`＝14 命中，去掉 4 条（`SymbolIndex.cpp:99` 正则、`.nproj` 三条）⇒ 本步 8 条、Task 6 保留名 3 条；跟着改的金样本实测为 `tests/e2e/manifest.txt:1010` 与 `test_module_import.cpp:1450-1452`（整句），`tests/CMakeLists.txt:1020` 留给 Task 6 | Task 5 新增 **Step 6b**（表格逐条给原文→新文）＋Task 6 Step 6 补保留名文案；Task 5 的 `git add` 补 `src/compiler` 与 `src/vm`（`SnMisc.cpp`／`NativeLibraryLoader.cpp` 原本落在清单外） |
| M | Task 7 Step 4 的数自相矛盾（记「12 处测试点」而列的是 8＋1＋2＝11），且生产面漏了 `.ui` 的 tooltip、`ProjectPropDialog.cpp` 的另外三条，测试面漏了 `test_projectmodel` 的 13 条与 `test_solutiontreemodel` | 两条精确 grep 重数：生产 **20 行／6 文件**（`.ui:61/64/67/70/279`），测试 **27 行／4 文件**（`test_projectmodel.cpp` 15 条，含 `:1093` 那条拿 `setNamespace` 当副作用的） | Task 7 Step 4 整块重写＋Files 补齐 |
| N | 两处指针错（轮 4 记 `ModuleRegistry.cpp:357` 是 `namespace` 闭合、`ModuleRegistry.h:203-208` 是 `OwnerOf`） | `grep -n namespace ModuleRegistry.cpp`：`:340` 开**匿名** namespace、`:357` 收它、`namespace nlang` 到 `:433` 才收；`h:203-208` 实测是私有的 `CompiledInFunctions`，`OwnerOf` 在 `:173` | Task 3 Files 重写：定义放 `:433` 之前（放匿名 namespace 里是 ill-formed，不是「位置不佳」而是编不过） |
| O | Task 6 Step 1 的块引用把 D5 诊断的出处写成「Task 4 Step 6」 | 计划内核对：D5 在 Task 4 **Step 10** 末尾（`VmBackend.cpp:133-136` 调用侧），Task 4 Step 6 是重复键诊断 | 引文改正 |
| P | `.new()` 拼写（轮 5 首条）：计划里 4 处用 `Box.new()` | 实跑 `ncc`：`Box p = Box.new();` ⇒ `main.n(line 3, char 15): Error: syntax error`；`grep -rln "\.new()" tests/e2e/*.n` 空 | 四处已改 `new X()`（含 `EmitExprNew.cpp` 的表行文字） |

**我这轮的独立实测（不是审阅者提的，是复核时撞出来的）**：

1. `q[0].x v;` 三种出路复现到两种（脏段名＋`bad allocation`），AV 未复现 ⇒ 轮 4 的结论「UB，
   进程内不可断言」加强而不是削弱；计划里的实测块已补这轮数字。
2. `temp/probe/base.output:33-37` 与 `d.output:34-40` 仍在树里且时间戳是本轮之前，冲突号可直接
   引用；**但两轮的状态号整体平移**（148/185/191/254/290 → 145/181/187/250/286），
   账本落地必须以当次构建的 `nlang.output` 为准，别把两轮编号混写。
3. `grep -rn "reserved for a library namespace" tests` ＝ **0 命中**，
   `grep -rn "collides with a built-in" tests` ＝ **2 命中**
   （`tests/CMakeLists.txt:1020`、`test_module_import.cpp:470-471`）⇒ Task 6 Step 6 的
   「跟着改的门」从「自己数」改成实测清单。
4. `test_debugger.cpp` 的锚点守卫注释只在 `:253` 一处（另两处 `:300/:348` 没有同款注释行），
   轮 4 记的三处断言位置 `:254/:300/:348` 与文案 `:256/:302/:350` 是对的。

**驳回的**：

- 「Task 4 的 Step 11（全量门）排在 Step 10（native 注册名）之前，所以 `native_*` 在声明的门里
  必然红」→ 读步骤序列：Step 10 在 `:1532`、Step 11 在 `:1577`，Task 4 全文只有 Step 11 一条
  `ctest`（`awk` 扫描 930–1660 行确认）。顺序本就正确，这条不成立。
- 「`(4a)` 应该也用目录型 fixture」→ `buildAndRun` 自己塞 `m_ImportDirs`，目录型是给
  `order.txt` 多模块用例用的，本条是单库单程序，不需要。
- 「Task 5 Step 6b 应该一次把保留名文案也改掉」→ 那 3 条句子的发射点是保留名表的
  `IsLibraryNamespace` 分支，表要到 Task 6 Step 5 才退役；在 Task 5 改一句马上要删的文案＝
  制造第二条规则源。

---

### 轮 6（三名新审阅者读轮 5 修订稿＋我自己按每条去树里／实跑复数）

这一轮的**共性**不是「哪里写错了」，而是**清点面的命令本身有毛病**：轮 5 之前的三条收口
命令（`grep … | grep -v '^\s*//'`、`cmake --build | grep -i shift/reduce`、
`grep -rn namespace docs`）都在**安静地少报**——少报的门比没有门更坏，因为它读起来像过了。

**接受的（每条写明复核方法；审阅者给的数字若与实测不符，落点写的是实测数）**：

| # | 发现 | 我怎么核实的 | 落点 |
| --- | --- | --- | --- |
| A | Task 5 的收口 grep 有两个毛病：① `grep -v '^\s*//'` 打在 `grep -rn` 的输出上是**空转**（行首永远是 `路径:行号:`）；② 范围既漏 `examples/`、`tests/fixtures/`，又把 C++ 的 `SnNamespace`／测试标签混进来。审阅者给的「轮 5 实测 4 个 e2e 命中全是注释」我复核为**属实**（`tests/e2e/import_io_missing.n:2`、`stdlib_reserved_names.n:1/5/6/8`、`stdlib_sqrt_canary.n:1`、`unresolved_import_call/lib.n:2` 全在注释里） | 把原命令原样跑一遍：`… \| wc -l` 加与不加 `grep -v` 都是 **7 进 7 出**；再跑「一把梭」`grep -rni namespace stdlib tests tools src include examples` ＝ **715 命中**（这份清单没人能勾，所以不能当门） | Task 5 Files 的收口块整段重写＝**两条各答一个问题**（磁盘 `.n` 源／C++ 里的内嵌源），基线 10 命中／8 文件 ＋ 24 命中／8 文件，另加「不改的三面」边界**（这两条基线数与「一把梭」的 715 全被轮 7 推翻：实测 10／**9** 文件、24／**6** 文件，一把梭按写死的七条目录是 **1297**（`-rni`）／1291（`-rnIi`），715 无法复现；原稿的 pattern (2) 还会漏掉缩进的嵌套外壳，已换成 `grep -rnE '" *namespace [A-Za-z_]'`。见下面的轮 7）** |
| B | `examples/` **全阶段没人扫**，而它是**随包发的** | `grep -rni namespace examples/` ＝ 4 命中／4 文件；`tests/packaging/verify_package.py:118-140` 的 `scan_public_text` 走 `docs/site`＋`examples` | Task 5 Step 5 补 3 条自述注释；`.nproj:2` 那一条推到 Task 7 Step 4（它跟着属性删）；收口命令 (1) 里加 `examples` |
| C | Task 5 Step 5 的 fixture 面数量对不上（轮 5 记「36＝20＋16」），且 `test_thirdparty.cpp` 被写成「4 条 `.n` 源写串」 | 逐文件 `grep -n`：`test_library_source.cpp` 22（10 内嵌源＋8 注释＋4 行 C++ 脚手架 `:39/:46/:48/:433` 不许动）、`test_thirdparty.cpp` 7 但**只有 `:66` 是内嵌源**、`test_native_loader.cpp:141` 是 **CHECK 标签**不是被钉的句子；合计 **54** | Step 5 换成逐文件清单；并写明 `mylib.n` 是**磁盘 fixture**、由 `tests/CMakeLists.txt:158-227` 喂给 ctest（不是内嵌串） |
| D | `test_module_import.cpp` 的 namespace 面漏了 `:941/:948` 两条源字面量，而 `:471` 被错分到这个面（它钉的是保留名整句，属 Task 6） | 读该文件 `:404 private slots:`／文件尾 `QTEST_GUILESS_MAIN` ⇒ 是 QtTest 槽，删槽**没有别处会报警**；`sed` 逐条看九行的正文 | Files 改列 9 行、`:471` 重划给 Task 6 Step 5 第 1 条；Step 5 加**槽处置表**（2 条改 scope／3 条删／各给已被哪条覆盖）＋「条目数与槽数是两回事」 |
| E | Task 5 Step 6b 只扫了小写 `namespace`，**6 条大写 `Namespace` 的文档行**全在外面；另有 4 个 Known-limitation 段落描述的就是本阶段要消灭的能力 | `grep -rn Namespace docs/{en,zh}` ＝ 6 行（`function-features.md:125`、`common-errors.md:66`、`declarations.md:64` ＋ zh `:117/:61/:57`）；Known-limitation 四块的行号从猜的 47-52/43-46/75-62 实测成 **47-53／43-47／73-77／60-63** | Step 6b 金样本表补 6 行＋整块删除 4 处，并立归属规则：**逐字引用诊断句的行归改那句话的任务，叙述性 `namespace` 归 Task 8** |
| F | Task 6 Step 5 把保留名写成「删一个函数」，实际是**一条规则跨三面**；`DuplicateFieldChecker.hpp` 的 `:280` 会被顺手删掉 | `grep -rn "is reserved for a library namespace" src` ＝ **2**；`CheckLocalNameReserved` ＝ **6 命中**（decl `StatementResolver.h:48`、def `StatementResolverDecls.cpp:42-50`、调用 `:261`／`StatementResolverFlow.cpp:81`／`:264`／`StatementResolverSwitchTry.cpp:248`）；读 `DuplicateFieldChecker.hpp:130-175` ＋ `:268-285` 确认 `ownerIsLibrary` 只在 `:147-156` 那块里用 | Step 5 新增第 4 条（整块删 `:140-156`、**保留** `:280` 别名检查并由 Task 7 Step 2 改名）＋第 5 条 fixture 整块改写（`use_mylib.n` 六行正文＋`mylib_nmod`/`_nocmake`/`_env_path` 三条 ctest 同提交翻转） |
| G | `tests/fixtures/native/` 的实测面与原稿不符：`SOURCE_FIXTURE_DIR` 定义了却没人用，`test_thirdparty.cpp:196-200` 根本不是读源点 | `grep -rn SOURCE_FIXTURE_DIR tests` ⇒ 只有 `tests/CMakeLists.txt:129` 的定义；真正的读点是 `:153/:171` 的 `mixlib.n`，`mylib.n` 走 CMake | Step 5 第 5 条按实测重写，并点名 `:196-197` 是 `} // namespace`（轮 5 我自己引错的行，一并纠正） |
| H | Task 6 的门里少文档门，且「改 `:1010`～`:1012` 三条金样本」这句把不归本任务的行算进来了 | `sed -n '1001,1020p' tests/e2e/manifest.txt`：`:1010` 是 `Namespace 'io'`（Task 5 Step 6b 的行）、`:1011/:1012` 分属两个**不同发射器**（`ModuleRegistryGate.cpp:100` 点分名／`ModuleBuilderImports.cpp:197` 单段外部名），不是重复样本 | Step 7 改名「全量＋e2e 语料＋文档门」＋补 `pytest` 命令；Step 6 第 1 条把两条发射器写成**逐字守卫** |
| I | Task 7 Step 4 的测试面是 27 还是 26 说不清（轮 5 两个数自相矛盾），`BuildEnvironment.cpp` 的行号写偏，`IsLibraryNamespace` 的调用点把已被 Task 6 删掉的算进来了 | 两条精确 grep 重数：测试 **26 行／4 文件**（9＋1＋15＋1，与逐条清单一致）、生产 20 行／6 文件；`sed -n '50,60p' src/compiler/BuildEnvironment.cpp` ⇒ 签名 `:55`、`{` 是 `:56`、调用 `:57`（原稿既写 `:57` 又写 `:55-57`，两处口径不同）；`grep -rn IsLibraryNamespace src include` ＝ 1 声明＋1 定义＋**7 调用**，扣掉 Task 6 Step 5 整块删的 2 处 ⇒ 本任务改 **5** 处 | Files 与 Step 2 同步为 26／`:55-57`／5 调用，并给出现场 grep 命令 |
| J | Task 7 的 `git add` 少 `src/tools/ncc`（Step 4 删的属性自述注释在 `ProjectFile.h:12/:18`） | 读 `src/tools/ncc/ProjectFile.{h,cpp}`：`ProjectFile.cpp` 只有 `:11/:13/:161` 三行 C++ `namespace`，**从不读该属性**；`ProjectFile.h:12` 的样例注释与 `:18` 的「IDE-facing … ignored by」两行才是本步要删的 | Step 6 的 add 补 `src/tools/ncc` |
| K | Task 8 的文档盘点门**大小写敏感**，会静默少 6 行；`mkdocs.base.yml` 根本没有 `nav:` | `grep -rn namespace docs` ＝ en 29／zh 5，`grep -rni` ＝ en 33／zh 8；`grep -n 'nav:' mkdocs.*.yml` ⇒ 只有 `mkdocs.en.yml:12`、`mkdocs.zh.yml:12`，base 里没有；`docs/translation-pending.txt` 为空 ⇒ `test_tree_parity.py` 的三条断言（`:58/:70/:79`）确实严格，但**严格的对象是页面清单／pending 名单／两份 nav 的路径集合，不是正文行**（轮 7 补：轮 6 把这条当成「双语正文的门」，于是三处「漏改就会红」的说法都建在一只不存在的门上，已逐处改成「静默面，靠清单勾」） | Step 1 换 `-rni` 并规定「开工时重取基线」；Files 的 nav 行改成两个语言 yml 的 `:34-48` 段 |
| L | Task 4 Step 8b 把 ndisasm 的文档查证推给 Task 8，但**没人查过 ndisasm 到底有没有门** | `grep -rn ndisasm tests/`（不带过滤）＝ **4 行**：`tests/CMakeLists.txt:859-862` 的 `version_ndisasm` ＋ `:867` TIMEOUT ＋ `tests/packaging/verify_package.py:54`。轮 6 在此记「3 行」是**我给自己的命令加了 `--include=*.txt/*.cpp/*.cmake` 把 `*.py` 挡掉**的结果——第 4 行不改变结论（`BIN_FILES` 在 `:188-201` 只做 bin 布局／存在性检查，从不执行 ndisasm），但**带过滤的 grep 不能当收口证据**；`grep -rn ndisasm docs/` 只有用法概要（en `cli-tools/ndisasm.md:11`／zh `:9` 的 `-func <name>` 是**占位符**）⇒ 文档面为**零行**，原推导成立但结论空 | 见下面「独立实测 1」：Task 4 新增 **Step 8c** 补门，并把 Task 8 名下那条查证结掉；Step 8c 的实测段同步为 4 行＋过滤教训 |
| M | 三条语法门都从**构建日志** grep 冲突数，而不改文法时那条命令是空输出 | 读 `src/compiler/CMakeLists.txt:15-21`：bison 只在 `DEPENDS nlang.y` 变化时跑 ⇒ 空输出在这道门上长得跟「无冲突」一样；`which win_bison`＝`/d/dev/win_flex_bison/win_bison`，直接跑报 `warning: 14 shift/reduce conflicts [-Wconflicts-sr]`、退出码 0、产物只落 `/tmp`（不动 `src/compiler/generated/`，工作树保持干净） | Task 1 Step 8／Task 2 Step 6／Task 3 的终门三处统一改成 `win_bison -d -o /tmp/p.cpp --header=/tmp/p.h …`（与 Task 5 Step 4 原本就用的写法一致） |

**我这轮的独立实测（不是审阅者提的，是复核时撞出来的）**：

1. **ndisasm 是 D13 唯一一处零门禁的用户可见面，而且它同时吃输入**：
   `src/tools/ndisasm/main.cpp:28/:93/:114` 打印表键，`:146` 的 `-func` 是
   `func.name != funcFilter` 的**精确比对**。实跑基线（现编 `tests/e2e/proj_same_dir`
   ＋全量转储 64 条 `function`）：
   - `function main`／`function f` ＝ 今天**裸名**的两条项目模块自由函数 ⇒ 本任务后是
     `main.main`／`extra.f`（这就是新门钉的东西）；
   - `function io.print … native, file=…\stdlib/io.n` ＝ 今天**已经带点**，Task 4 后不变
     （D12 的活证据，留着当对照，防「见点就算对」）；
   - **57 条 intrinsic 方法记录全是裸名且本来就重复**（`writeInt`/`readInt`/`length`/
     `toString`/`get`/`set` 各 2～3 份，分属 `ByteStream`/`FileStream`/`List`/`Dict`）
     ⇒ 方法按已定规矩保持裸名，所以限定化之后表里**必然同时有两种形状**，这不是漏改。
     也因此新门**不许**用方法名当过滤键（`ndisasm -func length` 今天就打印 3 条）。
   - 未匹配的 `-func` 打印完类表**照常 exit 0**（实测）⇒ 断言只能落在输出串上。
   ⇒ Task 4 新增 **Step 8c**：一条 `ndisasm_qualified_func_name`，挂在已有的
   `proj_same_dir_compile` 产物上（不新增 fixture），**本计划全篇唯一一条新增 ctest 条目**；
   失败侧由 Step 13 的关缝负控覆盖（补了第 3 步）。Global Constraints 随之改成**门限曲线**：
   Task 1～3＝63/63、Task 4 起＝64/64，Task 4/5/6/7 的四处期望值与 Task 6 Step 5 的
   「改名复用、总数不变」一起对齐。
2. **`.nproj` 的 `namespace=` 属性面被前两条 grep 完全看不见**：
   `grep -rIn 'namespace=' docs examples src tests tools` ＝ **18 行／15 文件**（轮 7 定稿：
   必须带 `-I`——不带是 20 行／17 文件，多的两条来自 `__pycache__/*.pyc`；轮 6 记的
   「18 行／16 文件」这一对数**复现不出来**（18 行只能配 15 文件），以 18／15 为准），其中
   文档样例 2（`docs/user_manual/{en,zh}/cli-tools/ncc.md:60/:55`）、`examples/hello_project/….nproj:2`、
   **7 份 `tests/e2e/proj_*/….nproj`（都是活输入**：`tests/CMakeLists.txt` 给每目录各挂
   `proj_*_compile`＋`_run`）、`test_projectfile.cpp:41`（只是输入字面量，**全文件零断言**）、
   `test_projectmodel.cpp` 4 行（`:623` 是**写侧** `contains` 断言）、
   `test_solutiontreemodel.cpp:30`、以及**文档门自己的写端**
   `tools/nlang-docs/src/nlang_docs/snippets.py:181` 的 `_NPROJ` 模板
   （`grep -rn '_NPROJ\|namespace' tools/nlang-docs/tests/` ＝ 0 命中，改它不动任何断言）。
   ⇒ Task 7 Step 4 加第三条命令＋逐条处置表，Files 补**这 12 行所在的 12 个文件**（docs 2＋
   examples 1＋e2e fixture 7＋`test_projectfile.cpp`＋`snippets.py`），Step 5 补文档门
   （改了 `snippets.py` 不跑 pytest＝收口门漏一条），Step 6 的 add 补 `docs examples tools`。
3. `ncc` 确实**从不读** `namespace` 属性（`ProjectFile.cpp` 的命中只有三行 C++ `namespace`），
   所以上面那 18 行删完照常绿——「过门但没人发现」正是这一面必须手动清的理由。
4. Task 5 Step 5 原本还留着一条 `grep -rni namespace stdlib tests tools src include examples`
   当「本步收口」，与我这轮量到的 1297 命中直接矛盾（同文件两处口径打架），已删。
   轮 6 当时记的 715 复现不出来（轮 7 按写死的七条目录重跑＝1297，带 `-I`＝1291），
   已连同 Files 收口块一起换成可复现的数。

**驳回的**：

- 「`ModuleNotFoundText` 的措辞应该在本阶段改成体现包名」——设计稿里没有这条决策，
  改了要打穿 `manifest.txt:1011/:1012` 两条金样本＋`test_module_import.cpp` 的整句断言，
  属于**未经批准的扩大战场**。两条金样本改写成守卫（逐字不动、必须仍然绿）。
- 「`Task 3 Step 1 (c)` 写成『先扫 root，扫不到再下钻壳』就能两种树都过」——那是本仓库
  明令禁止的兼容 shim，而且会让这条断言在两种树上「可能」绿＝它不再钉任何事实。
  改法是在 Task 5 就地换成 root 直取，`PackageOf`／`QualifiedName` 两句一字不改。
- 「ndisasm 的 `-func` 未匹配应该顺手报错」——那是这个工具**今天就有**的毛病，与包名无关；
  在本阶段改它＝往全仓改键的提交里掺无关行为。留给阶段 6 的工具面。
- 「`-func main` 加个裸名回落就不用改用户拼写了」——兼容 shim，且会给 D13 造第二个真相源。
- 「类表／结构体表也该加一条 ndisasm 门」——它们的键已由 Task 4 的
  `(5) TestBuiltinAndUserTypeNameCoexist` 在 `ModuleLoader::Load` 之后直接断言，
  ndisasm 只是 `std::cout` 同一批字符串；为一个 `cout` 再造 fixture 是空的。
- 「把 `SnNamespace`／`NK_Namespace` 一起改名成 Package」——包是**注册表里由路径派生的条目**，
  不是 AST 节点；unit root 仍然是这个类。这条如果做，就把 Task 4/5 的边界抹掉了。

---

### 轮 7（审阅者 A＝代码事实视角，读轮 6 修订稿；我按每条回树里／实跑复数）

**这一轮的共同形状变了**：轮 6 修的是「少报的门」（门不存在却当作存在），轮 7 的 8 条里
有 **6 条是我自己的 grep 命令写错**（`--include` 把 `*.py` 挡了、大小写与 `-I` 的取舍、
pattern 漏了带前导空格的行），也就是说「以 grep 结果为准」这句纪律本身需要一把尺子。
每条都重跑过，八条**全部属实**，其中第 5 条我换了一个比审阅者给的数更好的修法。

| # | 审阅者的说法 | 我怎么核实的 | 落点 |
|---|---|---|---|
| 1 | **[Critical]** Task 5 Step 6b 把 zh 的「已知限制」整条写作 `declarations.md:43-47`，实际是 **43-46**；`:47` 是另一条仍然成立的「导入目标的解析顺序」 | `awk 'NR>=41&&NR<=48'` 逐行打印 `docs/user_manual/zh/language-spec/declarations.md` ⇒ 43 起 `已知限制：`、46 末 `无效。`、47-48 是解析顺序条；en 侧 47-53 原稿写对 | Task 5 Step 6b 的行界＋「多删一行会发生什么」的说明（并纠正：没有门会响） |
| 2 | Task 5 Step 6b 把 en/zh FAQ 那段也列为「整条删除」，但段首的「`import` 只开放限定名」在本阶段之后**依然正确**，要删的只是中间的共享命名空间可达性句 | `awk` 打印两段（en `faq.md:73-81`、zh `:60-65`）⇒ 要删的句子**跨在行中间**（en 起于 `:74` 行内、止于 `:77` 行内；zh 起于 `:61`、止于 `:63`） | 该条拆成「可整条删除的」＋「只能改写的」两小条，并补两条收口 grep（死句 pattern 必须写成 `共享(的)?命名空间`，zh 原句带「的」） |
| 3 | Task 5 Step 5 第 5 条说磁盘两份 fixture「经 `ReadFixtureFile` 读进场景目录（`test_thirdparty.cpp:196-200`、`:163-167`）」——两处都不是读源点 | `grep -n 'ReadFixtureFile\|kMylibSource\|SOURCE_FIXTURE_DIR'` ＋ `grep -rni namespace tests/test_vm/test_thirdparty.cpp`（7 命中：`:66` 唯一源串、`:4/:163` 注释、`:37/:44/:46/:197` C++ 脚手架）⇒ 定义 `:153`、**唯一调用 `:171`（mixlib）**；mylib 走内嵌串 `:65`→写入 `:132-133`；磁盘 mylib 由 `tests/CMakeLists.txt:162-163` 拷贝、`:173/:185/:198/:203` 三条 ctest 消费；`SOURCE_FIXTURE_DIR` 只剩 `:30-31` 的 `#ifndef` 回落＝死宏 | Step 5 第 5 条按「两条不同消费路径」重写，mylib 面点名为**内嵌串＋磁盘两处**，并加死宏清理一条（`MIX_SOURCE_FIXTURE_DIR` 活的，别一起删） |
| 4 | 收口命令 (1) 的基线不是「10 命中／8 文件」 | 原样跑并 `awk \| sort \| uniq -c` ⇒ 10 命中／**9** 文件（`mixlib.n` 一个文件贡献声明行 `:9`＋注释行 `:4`） | Task 5 Files 收口块基线＋「为什么少算一个文件」 |
| 5 | **[Important]** 收口命令 (2) 的 pattern `'"namespace \|<< "namespace'` 实测只有 **22** 命中，不是 24 | 两条都跑：旧 pattern 22／6，新 `grep -rnE '" *namespace [A-Za-z_]'` **24／6**（10＋9＋2＋1＋1＋1）。漏的两行是 `test_module_import.cpp:941/:948` 的 `"    namespace B {\n"`——**引号后带缩进空格**，而这两行正是本任务要删的嵌套外壳 ⇒ 不采纳审阅者「改成 22」的建议，而是**换 pattern** 把数补回 24 | 命令本身＋基线＋一条「旧 pattern 漏在哪」的教训；原稿的「21 条内嵌源＋3 条文案＝24」分解是错的，删掉 |
| 6 | 「一把梭 grep＝715 命中」复现不出来 | 按写死的七条目录跑 `grep -rni`＝**1297**、`grep -rnIi`＝**1291**（逐目录 src 849／tests 233／include 193／tools 6／stdlib 6／examples 4）。715 换任何目录子集都凑不出来（`src/compiler include tests`＝796、`src include tests stdlib examples tools`＝585） | Task 5 Files 收口块＋Step 5 的引用＋轮 6 记录第 4 条，三处一起换成可复现的数并注明 715 不可复现 |
| 7 | `namespace=` 属性面的「18 行／16 文件」配不上 | `grep -rIn 'namespace=' docs examples src tests tools`＝**18 行／15 文件**；不带 `-I`＝20／17，多的两行是 `__pycache__/*.pyc` 的「Binary file … matches」 | Task 7 Files 的条目＋Step 4 的第三条命令（**定成带 `-I` 的形式**）＋逐条处置表头＋轮 6 记录第 2 条 |
| 8 | Step 8c 的「`grep -rn ndisasm tests/` ＝只有 3 行」是用 `--include` 过滤跑出来的 | 不带过滤＝**4 行**，第 4 行是 `tests/packaging/verify_package.py:54` 的 `'ndisasm.exe'`；查 `BIN_FILES` 的使用点 `:188-201` ⇒ 只做 bin 布局／存在性检查、从不执行工具，**结论不变、数要改** | Step 8c 的实测缺口段，并把「带过滤的 grep 不能当收口证据」写进去 |
| 附带 | 我自己顺着第 2／7 条复查时撞出来的两处（审阅者没提）：① Task 7 Step 4 测试组「27 行」与逐条清单 9＋1＋15＋1＝26 打架；② 我在全篇三处把 `test_tree_parity.py` 说成「双语正文的门，漏改就会红」 | ① 两条精确 grep 重数＝**26 行／4 文件**；② 读 `tools/nlang-docs/tests/test_tree_parity.py` 的三条断言（`:58` 页面清单、`:70` pending 名单、`:79` 两份 nav 路径集合）＋`test_two_column_css_is_mirrored` 比 CSS 字节 ⇒ **没有一条看正文行**；`test_snippets.py` 只解析 ```nlang 围栏与退出码 | ① Step 4 测试组改 26；② Task 5 Step 6b、Task 6 Step 7、Task 7 Step 4 三处的「否则文档门红」全部改成「静默面，靠清单逐条勾＋本仓库的成对文档约定」，并在轮 6 记录 K 上注明这条口径是轮 7 更正的 |

**驳回的**：无——这一轮 8 条全部核实为真，没有一条能驳。这本身就是个信号：轮 6 之后
留在稿子里的错**全是「我引用的证据是我自己用错开关的命令量出来的」**，而不是代码理解错。

### 轮 7（审阅者 B＝计划内部自洽／C＝测试风险，同批读轮 6 修订稿）

B 与 C 是并行派的两名新审阅者，视角与 A 不重叠：B 只读计划本身（前后条目配不配得上、
命令与期望串对不对得上），C 只读「按这份计划做会不会出现假绿／空跑」。
两人合计 10 条，**核实后采纳 9 条、驳回 1 条**。

| # | 来源 | 说法 | 我怎么核实的 | 落点 |
|---|---|---|---|---|
| B1 | B | Task 2 的 Files 没声明 `include/nlang/compiler/SnExpressions.h`，但 Step 3/4/8 都要改它 | `grep -n 'SnAsExpr' include/nlang/compiler/SnExpressions.h` ⇒ ctor 形参在 `:660`，访问器 `:675` 与成员 `:694` **已经是** `SnFieldExpr *`（`:672-674` 注释写明 Phase 13 为 alias 剪接放宽过） | Files 补条目，并按实测把「访问器与成员跟着放宽」改成「只放宽 ctor 形参」 |
| B2 | B | Task 6 Step 5 的 ctest 条目改名只说了 `add_test`，没说 TIMEOUT 名单 | 读 `tests/CMakeLists.txt:1034-1039` ⇒ `proj_reserved_segment_compile` 在 `:1038` 的 `set_tests_properties(… PROPERTIES TIMEOUT 180)` 名单里 | Step 5 第 2 条补「改名要同时拔两处」＋`grep -n proj_reserved_segment tests/CMakeLists.txt` 期望 0；Step 6 第 2 条的旧名引用同步 |
| B3 | B | 「`grep -rn namespace tests/e2e --include='*.n'` 的 4 个命中」复现是 7 | 原样跑 ⇒ **7 命中／4 文件**，逐行 `import_io_missing.n:2`、`stdlib_reserved_names.n:1/5/6/8`、`stdlib_sqrt_canary.n:1`、`unresolved_import_call/lib.n:2`（全在注释里，这点成立） | Task 5 Step 7 的补门说明＋轮 5 记录 D 行注「4 是文件数」 |
| B4 | B | Task 5 Step 5 的坑①说 `stdlib_reserved_names.n` 那 4 行会被两条收口 pattern 命中 | `grep -n "namespace [A-Za-z_]" tests/e2e/stdlib_reserved_names.n` ＝ **0**；pattern (1) 的全量命中集里也没有它（10 命中／9 文件的清单可逐条对上）⇒ 命中它的是**宽 grep**（无 ` [A-Za-z_]` 约束） | 坑①重写：标明归属、把两个方向的误读都写出来（宽 grep 当收口＝以为要改；(1)/(2) 当收口＝把「没命中」读成「已改完」），并指到 Task 6 Step 5 第 3 条 |
| B5 | B | Task 7 Step 4 的 `namespace=` 基线 18／15 是在 `d7ca710` 量的，而本任务在 Task 6 之后跑 | `grep -rIn 'namespace=' docs examples src tests tools` ⇒ 18／15，其中第 9 行正是 `tests/e2e/proj_reserved_segment/proj_reserved_segment.nproj:2`，Task 6 会整目录改名 | Files 的该条补「开跑时重量为准，18 不是验收值」＋改名后两种落点（18／15 或 17 行／14 文件） |
| C1 | C | Task 4 Step 13 的负控在 `-R` 上会**假绿**：名字拼错时 ctest 不报错 | 实跑 `ctest --test-dir build-dev/tests -C Release -R nosuchtest` ⇒ 打印 `No tests were found!!!` 且**退出 0**；`-N -R` 才给出条数 | Step 13 重写：先 `-N -R` 数出 `Total Tests: 2` 再跑，期望串写死 `1 tests failed out of 2`；删掉不可达的「Not Run ⇒ DEPENDS 没生效」句 |
| C2 | C | Step 13 的期望名 `test_stdlib`／`test_native_*` 不是 ctest 条目名 | `grep -n 'add_test(NAME' tests/CMakeLists.txt` ⇒ 条目是 `library_source_tests`（`:150`）这类名字，可执行目标才叫 `test_library_source` | Step 13 的第 2／4 步改用真实条目名并点出「目标名≠条目名」这条陷阱；Global Constraints 的 `-R` 语义块加第 ④ 点 |
| C3 | C | Step 13 只 `--target ncc`，进程内测试二进制不会带上关缝后的代码 | 读 `src/vm/CMakeLists.txt`（`Register.cpp` 编进 `nlang_vm`）＋ `test_library_source.cpp:435-451` 的 `main()` 是进程内 compileRun、`return g_fail > 0 ? 1 : 0` ⇒ 静态链接，不重链接就跑旧代码 | Step 13 两处构建命令补 `test_library_source`（注：若负控同时骑 `test_debugger` 的断言，target 再加它） |
| C4 | C | 全篇五处「`git status --porcelain` 期望：空／必然非空」在本工作树里恒成立或恒不成立 | `git status --porcelain` ⇒ 只有 `?? main.n`；带目录参数（`-- src tests tools docs stdlib examples`）⇒ 空 | Global Constraints 立基线口径，五处措辞逐条改成「除基线行 `?? main.n` 外必须空」，Step 13 那处再加恢复侧的 `ctest -R library_source_tests` |
| C5 | C | Task 5 把 `test_highlight.py:77-90` 列为「同步修改」，但那条断言两边同源，正常做完不会红 | 读 `:81-90` ⇒ `scanner` 从 `nlang.l` 现拉，断言是 `scanner == (KEYWORDS\|TYPES\|CONSTANTS) - docs_types`；Step 2 删扫描器、Step 6 删 lexer 集合一起缩 ⇒ 保持绿 | Files 改成「**不改**」＋写明它只抓「一边动了」、真正的负向证据是 Step 1 的解析失败测试；`highlight.py` 的注释计数 40→39、54→53 补进行号 |
| C6 | C | Task 5 让 Step 6 顺手删 `check_source_size.py:38` 的 `namespace` | 读 `:38-46`＋`:79` ⇒ `KEYWORDS` 只用于 `line.lstrip().startswith(tuple(k + " " …))`，是**把 C++ 的 `namespace nlang {` 从函数定义判定里排除**的启发式；`SIG_RE` 要求行含 `(`，`namespace X {` 走不到这一分支 ⇒ 删它无可观测差别且方向错 | Files 改「**不改**」并写理由；Step 6 的「同」一句删掉，换成「不在本步范围」 |

**驳回的（1 条）**：B/C 里有一条建议把 Step 13 第 2 步写成
`cd build-dev/tests/Release && ./test_library_source.exe`——**不采纳**：手动进构建目录跑二进制
绕过了 ctest 的 `-R` 计数防呆，正是 C1 要修的那个洞（名字／路径写错时没有任何东西会响）。
改成 `-N -R library_source_tests` 数出 1 条、再用 ctest 跑，退出码由
`test_library_source.cpp:451` 的 `return g_fail > 0 ? 1 : 0;` 保证传给 ctest。

**这一轮的形状**：B 与 C 的 10 条没有一条是「代码理解错」，全部是**计划自己的引用失效**——
改名只改一处、grep 用了宽窄不一的两条命令、门限口径依赖一个没写下来的缓存开关、
把「目标名」当「ctest 条目名」、把只检查清单的门当成检查正文的门。
A／B／C 三人同轮的 18 条里有 **7 条**落在「命令开关」这一类，因此 Global Constraints
新增了两条通用尺子（`-R` 的三条语义、`git status` 的基线行），后面每一轮先按尺子量，
不再靠 reviewer 逐条撞。

---

### 轮 8（改用逐任务循环：三名新审阅者只读 **Task 1 段落**）

用户 2026-09-30 定了新节奏：**审核单位从「整份计划」换成「一个 task 的计划段」**——段内循环到
没有新问题就实现，实现后再对 diff 循环，然后进下一个 task。非关键问题允许推到下一个 task 的
循环里结。本轮就是这一形状的第一次：A＝计划内部自洽、B＝按段执行会不会假绿、C＝测试风险。

**接受的（A 9 条／B 8 条，落点全在 Task 1，逐条带复核命令）**：

| # | 来源 | 发现 | 我怎么核实的 | 落点 |
|---|---|---|---|---|
| A1 | A | Files 的账本替换范围 `:1218-1246` 照字面会**删掉一条产生式** | `sed -n '1246p' src/compiler/grammar/nlang.y` ⇒ `NameExpr: IdentifierExpr { $$ = new SnNameExpr($1, @1); } ;`；`sed -n '1216,1220p'` ⇒ `:1218` 才是注释首行 | 范围改 `:1218-1245` ＋ 收口命令 `sed -n '1246p' …` 必须仍看到这条产生式 |
| A2 | A | `:1218-1228` 声称「剩下的一条 rr 在 `<` 上」，与本任务的新数字矛盾 | 直接跑 bison：只有 `warning: 14 shift/reduce conflicts`，**无 rr 行** | 起点从 `:1229` 提到 `:1218`，整块按实测重写（Step 7 第 1 条） |
| A3 | A | Step 3 只换函数体 `:160-172`，替换体自带新注释 ⇒ 旧注释 `:155-159` 五行留在头上 | `sed -n '152,176p'` 打印确认注释／函数边界 | 替换范围改 `:155-172` |
| A4 | A | Step 3 的新注释里引用 `temp/probe/a.y`——**不入库的路径**，提交后没人能找到 | `git check-ignore -v temp/probe/a.y` ⇒ `.gitignore` 命中 | 注释改写成「a 2026-09-29 probe grammar」，把实测结论留下、把出处换成不可失效的描述 |
| A5 | A | Step 6 的替换体 11 行 vs 锚点 9 行 ⇒ 498 行的文件净 ＋2 顶到 500，把余量吃光且不留给 Task 2/5/6 | `wc -l src/compiler/builder/ExprResolverTypes.cpp` ＝ 498；`sed -n '332,340p'` ＝ 9 行 | 替换体压成 9 行（净零），并把「顶格不算越界但没余量」写进阶梯 |
| A6 | A | 账本注释是**口算**的，注释记 12 sr＋1 rr 而实测 14 sr＋0 rr；承诺的「build log notice 当漂移信号」已经漏掉过一次 | 复跑 bison；并复测 `%expect` 语义（见 A7） | Step 7 改成机械门：序言加 `%expect 14` |
| A7 | A | （A6 的反驳前提）旧注释说「`%expect` 钉不住，因为那条 rr 会让它因 rr 报错」 | 造一条 1 rr 的合成文法实测：`%expect 0` ⇒ `error: reduce/reduce conflicts: 1 found, 0 expected`、rc=1；`%expect-rr 1` ⇒ `warning: %expect-rr applies only to GLR parsers`、rc=0。**旧说法在它成立的时代是对的**，rr 归零后前提消失 | Step 7 第 2 条把两半实测都写进去，避免下一个人以为注释在撒谎 |
| A8 | A | 余量表把 `SnExpressions.cpp` 记成「Task 1 Step 5 在动」 | Step 5 改的是 `include/nlang/compiler/SnExpressions.h`（`allowlist.json` 第 1 条整文件豁免）；`grep -n 'SnExpressions\.cpp' docs/dev/phase5_plan.md` 在改前**只命中这张表自己**；`SnNamespace::` 定义实际在 `src/compiler/SnMisc.cpp`（255 行） | 表行按实测改写（轮 9 落地，见 B9 同一条），Task 1 的 `git add` 不含 `allowlist.json`（Step 9 复核为已经不含） |
| A9 | A | Produces 少列 `compileDir`，而 Task 2/4 的进程内用例直接吃它 | 读 Task 2/4 段落里的 `compileLog(dir, {…})` 形状 | Produces 补齐三个名字 |

**接受的（B）**：

| # | 发现 | 我怎么核实的 | 落点 |
|---|---|---|---|
| B1 | Step 1 的 helper 块引用了**文件里不存在**的 `DrainLogger`／`checkAndCompile`，而 `compileLog` 在同一步给了两种互斥签名（一参 `const std::string&`、两参 `fs::path`）——正是该步自己 :394 立的规矩「不要造文件里不存在的东西」 | 读 `tests/test_vm/test_library_source.cpp:60-100` 的真实形状 ＋ `include/nlang/compiler/Logger.h:97-119`（`ListCompileLogger` 只有 `cbegin()/cend()`，元素是 `CompileLogItem*`，取 `Message()`） | 三个 helper 全部给字面定义（`runBuild`／`compileLog`／`compileDir`），`compileRun` 加第三个默认实参 |
| B2 | 「文件里现有的 11 处单参调用点」数错 | 逐条 `grep -n`：`compileDir` 5 处（`:91/:144/:153/:169/:429`）、`compileRun` 7 处（`:133/:189/:245/:296/:339/:373/:407`）＝ **12** | Step 1 注按实测数改写，并写明「加默认实参 ⇒ 12 处一行都不用动」 |
| B3 | 目录型 fixture 的 `alib.n` 内容没有逐字给，执行者会照 `kLibSource` 手抄而漏掉函数 | `sed -n '104,116p'` 打印 `kLibSource` 正文（`twice`／`quad`／`fact` 三个函数＋`namespace alib {`） | Step 1 给逐字 `alib.n`，并注明「两条用例各留一份，别做成共享 fixture」 |
| B4 | 「runner 先找 `<name>.n` 再找 `<name>/`」这句判据错，顺序其实相反 | 读 `tests/e2e/run_e2e_tests.py:159-161`（联合判断，无先后）、`:174`（examples 清单拒绝目录型）、`:188`（`if isdir` 分支**排在**单文件路径之前）⇒ **目录赢** | 判据改写 ＋ 给出「万一建出目录」的症状串 `FAIL qhead_index (missing order.txt in test dir)` ＋ 收口 `ls -d` 必须恰好一条 |
| B5 | 参照形状 `native_crossmod/` 的 `order.txt` 末段不叫 `main`，直接照抄会连累 `:269` | `cat` 该文件 ⇒ `nativelib nativemain`；`run_e2e_tests.py:269` 跑的是 `modules[-1]`（**位置**而非名字 `main`） | Step 1 补「位置才是入口判据」一句 |
| B6 | Step 2 的红 Run 没有任何东西防「新加的 `Test*` 函数忘了注册进手写 `main()`」⇒ 编掉、门空转 | 读 `test_library_source.cpp:435-451`（`main()` 逐条调用、`return g_fail > 0 ? 1 : 0`） | Step 2 加注册对账第 0 步（与 Global Constraints :66-71 同形） |
| B7 | Step 2 缺 SKIP 计数（漏一个 manifest 名字不红、只 SKIP） | Global Constraints 轮 7 已立这条尺子，Task 1 的块没照做 | `tee /tmp/e2e_step2.log` ＋ `grep -c '^SKIP'` 期望 0 |
| B8 | UB 出路面只列三种，漏的那一种**最像已经修好了** | 轮 8 复跑 `alib.twice(1) v;` 实测到 `Type '<乱码>' is not a member of namespace 'alib'`——读起来像正常语义诊断 | Step 2 期望补第 ③ 面 |
| B9 | 轮 8 在此只列到「四种出路」，但把不稳定的东西写成了计数 | 轮 9 后台审阅者独立 5 次未重现 ④（AV），我自己 5 次里 2 次 ⇒ **同一形状按堆状态漂移** | 轮 9 删掉「几次」，判据收成「不含期望串」一条（见下） |

**驳回的（C 整份报告）**：C 报的 12 条里，被引用的标识符**全部 grep 零命中**
（`expectNvs`／`expectNs`／`expectIdent`／`expectNativeIntDeclOnly` 各 0 命中），
其引的行号（`nlang.y:1315`）与引的用例文本（`a.c.f(1) b`）与树内实际内容不是同一段。
⇒ **整份按「未对着树写过」处理，不采纳**；唯一与 A/B 重叠的一条（「`%expect` 会让 bison 静音」）
就是 A6/A7 已经覆盖的那条。这条要单独报给用户：**审阅者会编造引用，所以每条都要先 grep 再落**。

### 轮 9（后台审阅者＝Task 1 测试风险视角，读轮 8 修订稿；与用户新节奏下的一次实现前复核同批）

这一轮由一个**并行后台 agent** 返回（不是我主动等的，回来时我已经在改同一份稿子，它的引用
因此有两条对不上我改完的文本——下面按当前文本重新对号）。**7 条：采纳 5、其中 1 条前提被证伪、
2 条 Minor 按用户「非关键可延后」的指令记录不落地**。

| # | 发现 | 我怎么核实的 | 落点 |
|---|---|---|---|
| F1 | **[Important]** Task 1 Step 8 的冲突门期望还写着「stderr 一行 `warning: 14 shift/reduce conflicts`」，而 `%expect 14` 在位时 bison **零输出**；照字面执行的人会判红并可能把 `%expect` 删掉 | 实测：`sed '8a %expect 14' … \| win_bison` ⇒ rc=0、无任何输出 | **在审阅者返回之前我已经翻转了这条**（轮 8 尾部），此处记为「同一缺陷、两条独立路径命中」，并据此把「形状从读到文本变成读不到文本」写进 Step 8 |
| F2 | **[Important]** Step 2 的注册对账期望串说「零行 UNREGISTERED 输出」，但那条命令**没有** Global Constraints 版的 `| sed "s|^|UNREGISTERED $f: |"` 尾巴 ⇒ 输出的是裸函数名，按字面找关键词的人会在满地红上读出绿 | 原样跑该块 ⇒ 输出 `TestQualifiedTypeHeadKeepsLegalMemberAccess` 而无 `UNREGISTERED` 前缀（注入对照后）；干净树上 defs 11／bare 11、输出为空 | 命令补成与 :66-71 **逐字一致**，期望改写成「零行输出；任何一行都是没注册的函数名」 |
| F3 | **[Important]** 「哪一面出路出现几次」是不可复现的：④（AV）审阅者 5 次全未重现，我 5 次里 2 次 | 两边都是同形状多次重跑，差异本身即证据 | Step 2 删掉全部次数，明写「**门的判据只有一条**：任何一面都不含期望串」，并补 `grep -rn 'Malformed qualified type reference' src include tests tools docs` ＝ **1 命中**（`ExprResolverTypes.cpp:338`）⇒ 红 Run 不可能碰巧匹配上；④ 降级为「不是断言的一部分」 |
| F4 | **[Important→前提证伪]** 红 Run 会往 `tests/e2e/qhead_call/` 留 `.nmod`，「trips the mandated `git status --porcelain` gate」 | `git check-ignore -v tests/e2e/qhead_call/alib.nmod` ⇒ `.gitignore:33 *.nmod` **命中** ⇒ 不进 porcelain，审阅者的前提不成立。但 mismatch 分支确实不删（`run_e2e_tests.py:238-244` 直接 `continue`，只有 :247-251 的 PASS 分支删），而 `:221` 用 `isfile(out)` **代替退出码**判成败 ⇒ 上次留下的 `.nmod` 会把「这次编译失败」读成「编过了」 | **结论改、动作留**：Step 2 红 Run 后加一行 `rm -f`，并把「为什么清」写成 `:221` 的假象而不是「树脏」，同时把审阅者那条误判连同 `check-ignore` 证据一起记下来 |
| F5 | 三条负例**全部落到解析端**，语法层零改动；Step 6 的 `segs.size() < 2` 今天不可达 | 把 Step 3＋4 的替换体打进文法副本再跑 bison ⇒ 仍 14 sr；`ncc build nosuch.Box b;` ⇒ 单条干净诊断、无 `.nmod`；`ModuleBuilder.cpp:107-108` 在 `HasError()` 上早退 | Task 1 Step 1 后新增「轮 9 定层」段：给出上面两条实现期不必重查的事实，并规定 `segs.size() < 2` 是**今天既有的**守卫、按「只加一个条件」保留、不得当新增死码删 |
| F6 | Minor：进程内对照正例今天就是绿的、修完也绿，只防「守卫开得太宽」，碰不到 `CollectQualifiedSegments` | 属实（`ncc build qhead_ok/main.n -I qhead_ok` ⇒ Compiled successfully、rc=0） | **不落地**：它的存在理由正是「若红则说明本任务的守卫越界」，这条价值不依赖它今天会红。按用户「非关键可延后」记录 |
| F7 | Minor：账本注释那一半**没有失败能力**（Task 1 不改产生式，14 永远等于 14）；`%expect` 本身受 mtime 门控 | 属实；但直接跑 `win_bison` 与构建解耦，不受 mtime 影响，这条已由轮 6 的写法解决 | 与「`%expect` 是唯一新增机械牙」一并写进 Step 1 的定层段（F5 落点里那句「Step 7 改的只是账，唯一的机械新增是 `%expect`」），不另开条目 |

**轮 9 的连带发现（审阅者没提、我顺着 F1 往回走时撞出来的，全部实测）**：

1. **`%expect` 把「门禁」和「测量」拆成两件事，而全计划有四处语法门只会读退出流**：
   Task 1 Step 8、Task 2 Step 6、Task 2 Step 7c、Task 5 Step 4。`%expect` 一落地，这四道的
   期望文本（「14 shift/reduce」「16 shift/reduce」）**全部读不到数字**。统一改成：
   门禁＝`win_bison … && rc==0 且 stderr 空`；测量＝`--report=all` ＋
   `awk '/[0-9]+ conflicts: [0-9]+ shift\/reduce/{n+=$4} END{print "sr="n}'`。
2. **`$4` 不是 `$3`**：`.output` 行形 `State 148 conflicts: 1 shift/reduce`，`$3` 是字面量
   `conflicts:`。用 `$3` 实测得 `sr=0`——**一条永远说「零冲突」的测量命令**，比没有门更糟。
   这是我自己写这条命令时先撞后改的，不是审阅者给的。
3. **`%expect` 不匹配时 `.output` 照样写**：在 16 冲突的副本上留 `%expect 14` 跑
   `--report=all` ⇒ `error: … 16 found, 14 expected` ＋ 一条
   `note: rerun with option '-Wcounterexamples'`、**rc=1**，但 `sr=16` 仍然量得到。
   这条解掉 Task 2 的顺序死结（先量后 bump 安全，但两者必须同提交，否则 `cmake --build` 红）。
4. **Task 1 Step 7 里那句「轮 8 已挂进 Task 2」是指向空气的交叉引用**：
   `grep -n '%expect' docs/dev/phase5_plan.md` 的命中集在改之前**全部落在 Task 1 段内**，
   Task 2 全文 0 命中。已真挂（Task 2 Files 第一条），并把那句假承诺改成
   「轮 8 写过一句假的、轮 9 实测为假、现在真的挂上」。
5. `nlang.y` 账本块的行界再次复核：`:1216-1217` 是 `FA_Default` 产生式尾，`:1218` 起注释，
   `:1245` 收，`:1246` 是 `NameExpr:` 产生式——前置事实里原引的 `:1229-1245` 一并改成 `:1218-1245`。

**这一轮的形状**：F1 与我的轮 8 尾部改动**同时命中同一处**，说明「翻转期望文本」这类改动
在计划里是**广播型的**——一处机制变了，所有引用它的门都要跟着变。轮 9 因此把四处统一收口，
并把「口算次数」全部降级成「判据只有一条」。C 的编造引用与 F4 的「前提看着对、开关一跑就废」
是同一种病的两面：**引用必须先落地成命令，命令必须自己带开关**。
