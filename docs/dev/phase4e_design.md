# Phase 4e 设计（路径就是名字）

> **已作废（2026-09-29）**：阶段重划后由 `docs/dev/phase5_design.md` 取代，划分见
> `docs/dev/phases_567_design.md`。保留本稿只为记录命名模型的推演过程。三处实质错误以新稿为准：
> ① 本项目模块的成员今天挂在 root、函数键也是裸的（`ModuleRegistry.cpp:293-300/377-385`
> 的 `isLibrary ? 容器 : root`），所以「项目侧键全部变长」是本阶段最大的一面，不是附带影响；
> ② 「宿主 native 先查限定名、回落裸名」是兼容垫片，改为测试注册表直接写限定名；
> ③ `.nlib` 改名 `.npack`，且程序与库共用这一个扩展名。
>
> 状态：设计稿（docs/dev/，随分支跟踪；2026-09-30 自 temp/ 迁入）。分支：dev（worktree `E:\cases\nlang\dev`）。行号基线 `d7ca710`。
> 上游：`docs/dev/phase4b2_audit_notes.md` 第 1 轮 Important 2/3/4。
> 用户裁决（2026-09-29，逐条覆盖本稿早期版本）：
> ① 开 4e「类型标识」，先于 4d；② **不考虑向下兼容**；
> ③ **包名只由路径决定**：相对项目源码根（库则相对搜索根）的点分路径，
>    **不加 `package` 关键字**；④ **`namespace` 关键字删除**，分组只有目录；
> ⑤ 一个源文件 ＝ 一个包；一个项目 ＝ 一个可打包的库；
> ⑥ 产物分层：`.nmod` ＝ 一个源文件的编译产物（对应 C/C++ 的 `.o`、Java 的 `.class`），
>    `.nlib` ＝ 一个项目打包成的库（对应 `.jar` / `.lib`），链接步骤存在（工具面另议）。
> 未决（本稿给了默认，等用户裁决）：
> 一、**落地顺序** 4e 只做名字 → 阶段 5（对象文件＋`.nlib`＋链接）→ 4d 排在其后；
> 二、**4e 期间带点导入名怎么落地**（默认：只解析到源码库，预编译 `.nmod` 仍按「文件名＝导入名」，
> 带点名字落到产物文件上是阶段 5 `.nlib` 成员表的事）；
> 三、**链接后的可执行产物叫什么**（今天 `ncc run <module.nmod>`，`ncc/main.cpp:39`）。

> 探针证据：`temp/rb6/RESULTS.md`（rb6/rb7/rb8，全部真实 ncc 运行）、`temp/rb3/`（同名类型串布局）。

## 0. 根因：两处

**第一处：名字的来源没有Declared过，全靠巧合。**
`DeriveModulePath`（`src/compiler/builder/ModuleRegistry.cpp:48-65`）在项目根内给出
相对根的点分路径（`utils/helper.n` → `utils.helper`），根外或没有根时**退化成文件名**
（`io.n` → `io`）。库文件全走退化档，于是「一个库要能用」需要三个东西恰好相等：
文件名 ＝ 文件里那层 `namespace` 名 ＝ 用户写的 `import` 名。
stdlib 的 `stdlib/io.n` ＋ `namespace io`（`stdlib/io.n:5`、`math.n:5`、`fs.n:5`）刚好满足，
所以一直绿。不满足时该单元**两种拼写都不可达**（rb6 B/C/D/E/F/G、rb8 P1/P2/P7），
而编译器还会建议一条无法执行的 `import gfx.color;`——库发现明确跳过带点的名字
（`src/compiler/ModuleBuilderImports.cpp:73` 注释、`:143/173-178`）。

**第二处：VM 里函数用全名、类型用裸名。**
`src/vm/backend/Register.cpp:24-41` `QualifiedFunctionName()` 产出 `io.print`；
`Register.cpp:47/68`、`RegisterClass.cpp:95/100` 却用 `sn.Name()` 写类型表，
查找 `include/nlang/vm/CompiledModule.h:314-326` 首个匹配、零重复检测。
`alib.Point` 与根 `Point` 同时在场 → 代码拿到先注册者的字段布局，无诊断（`temp/rb3/` 已复现）。

本阶段修第二处，并用第一处的新规则把名字钉死。

## 1. 模型

```
<项目源码根>/utils/helper.n      → 包 utils.helper   → import utils.helper;  utils.helper.help()
<搜索根>/gfx/color.n             → 包 gfx.color     → import gfx.color;      gfx.color.Rgb
<搜索根>/io.n                    → 包 io            → import io;             io.print(...)
```

| 概念 | 由什么决定 | 文件里写什么 | VM 表里的名字 |
| --- | --- | --- | --- |
| 包 | 相对起点的目录路径 ＋ 文件名 | **什么都不写** | 作为名字前缀出现 |
| 分组层级 | 目录层级（`namespace` 删除） | 只有目录 | — |
| 函数 / 类型 | 声明名 | 声明本身 | **包名 ＋ "." ＋ 名** |
| 类/接口/枚举的成员方法 | 声明名 | 声明本身 | **恒裸名**（分派依赖：`Register.cpp:20-23`、`VmExecutorOpsCalls.cpp:300`） |

起点两处：项目 ＝ `.nproj` 所在目录（`ncc/main.cpp:284` 已经在填）；库 ＝ `-I` 搜索根
（`ncc/main.cpp:56-58`）。`ncc one.n` 单文件编译时根就是该文件所在目录，结果与今天的
退化档相同（名字＝`one`），但不再是额外特例。

**公开拼写逐字不变**：`io.print`、`utils.helper.help()`（e2e
`tests/e2e/proj_import_visibility/main.n` 现状）、4b-2 已落地的 `sub.thing.Box` 全部保留。
变的只有一件：这些名字从此**只有一个来源**，不再有第二处可以互相矛盾。

### 1.1 为什么不加 `package` 关键字，为什么删 `namespace`

`package` 唯一的价值是「文件挪了位置名字不变」。它的代价是名字有两个来源，
于是需要一条优先规则、需要一致性检查、需要解释「声明和目录不一致时听谁的」。
用户裁决：名字跟目录走，移动目录就是移动接口——这与项目侧今天的行为一致
（现在也没有任何办法改目录而不动 import 语句）。

`namespace` 删掉之后，目录成为唯一的分组手段，第一处根因（三重巧合）不再有写法：
文件里没有任何可以「恰好等于文件名」的东西了。分组要变细就加目录，
`a/b/c.n` 与 `a/b/d/x.n` 是两个包，名字由路径给出，无需任何人声明。
AST 里的容器节点保留（编译器仍需要它挂成员），只是用户写不出来。

### 1.2 限定符是包名原文，不做末段简写

路径派生的名字末段天然重复（`utils/helper.n` 与 `core/helper.n` 都是 `helper`），
只写末段就得新增冲突诊断，而唯一出路是 alias 导入语法——那是本阶段之外的预算。
取原文（Java 的冲突处理也是「写全名」）后：`a.io.f()` 与 `b.io.f()` 同场互不串，
无需 alias、无需冲突规则，源码里写的、VM 表里存的、报错信息里显示的永远是同一个串。

### 1.3 产物分层（4e 只承认第一层，其余是阶段 5）

| 层 | 内容 | 状态 |
| --- | --- | --- |
| `.nmod` ＝ 一个源文件的产物 | 一个包的代码 | **阶段 5**。今天是整项目一份合并产物（`ncc/main.cpp:263-273`），AST 在产码前就被 `MergeTransUnits` 合掉了（`ModuleBuilder.cpp:233-258`） |
| `.nlib` ＝ 一个项目打包成的库 | 归档，成员名＝包路径 | **阶段 5**。链接机制的一半已在跑：`Import.cpp:164-168` `RemapBytecode` 补 11 类按下标的操作数，`TypeDesc.h:90` 重编 struct/class 下标，`Import.cpp:231` 起每模块一张 `PerModuleRemap` |
| 名字唯一 ＋ 类型也用全名 | 本阶段 | **4e** |

4e 与阶段 5 的依赖关系是单向的：未解析引用要按名字记账、链接时定址，
名字必须先是唯一的、类型与函数同规则的——所以 4e 先做，且做完阶段 5 不需要回头改 4e 的规则。

## 2. 规则（每条一处实现）

### R1 名字：`PackageName(moduleIndex)` ＝ 相对起点的路径

- 一个函数产出名字，删掉「根外取文件名」的特例；根缺失时（单文件编译）根＝文件所在目录。
- 起点不唯一时**报错不排序**：两个 `-I` 根给出同一个包名 → 诊断（今天 `Import.cpp:237/243/248`
  先到先得、静默吞掉）。这条检查同时取代「目录名不许叫 io/math/fs」的保留名表
  （`ModuleRegistry.cpp:78-95`）——撞名有真检查后，专门的表是多余的第二套规则。
- 容器解析一处实现 `PackageContainer(moduleIndex)`：从 root 按点分段下降取或建，
  成员挂上去。收拢今天重复的判定：`CompiledInFunctions`（`ModuleRegistry.cpp:280-311`）、
  `FindModuleType`（`:359-397`）、`Access(SnIdentifierExpr)` 的类型分支
  （`ExprResolverTypes.cpp:425-433`，今天对类型是 owner 盲的）、`ResolveClassBases`
  （`StatementResolverTypes.cpp:27-56`）、流式类型实参链走（`ExprResolverMemberBuiltins.cpp:306-310`），
  共 5 处。**不动**：`ProbeNonFunctionField`、`FindFieldInUsings`、`CheckUnitAliases`
  （成员/别名可见性，与类型身份无关）。
  `pRoot->FindField(path)` 今天是单段字典命中（`src/compiler/SnMisc.cpp:34-37`，无递归），改为逐段下降。

### R2 类型与函数共用一条名字规则：`QualifiedName(const SyntaxNode&)`

- 把 `Register.cpp` 私有的 `QualifiedFunctionName()` 提为公开缝，拆成「路径段收集」＋
  两个入口（函数、类型），拼写从此不可能漂移。
- `CompiledStruct::name` / `CompiledClass::name` 存限定名；
  **泛型擦除基名不变**（`BaseName()` 的 `"List"`/`"Dict"`，`SnMisc.h:307-309`、`EmitExprNew.cpp:55`、
  `EmitExprInitList.cpp:305-307`）；内置类型与异常名不变（`VmExecutor.cpp:26/32/38-47`）；成员方法仍裸名。
- 24 个读取点分两类：AST 在手 → `QualifiedName(*decl)`；字面量/擦除名 → 原样。
  延迟字段类型名表（`Register.cpp:63/86/101`、`RegisterClass.cpp:113/117/161`）
  写入端与解析端**同提交**改；`declMap`（`RegisterClass.cpp:93/100/157`）改限定键；
  `Import.cpp:237/248` 的去重按限定名——外部 `alib.Object` 不再并入内置 `Object`
  （今天 `:241` 注释把这当特性），用户根级 `Object` 仍并入。
- 注册时同键重复 → 诊断（今天 `Register.cpp:47`、`RegisterClass.cpp:95` 无条件 push）。
- 磁盘与序列化跟着变：`ModuleSaver.cpp:178-180/223-225` ↔ `ModuleLoader.cpp:245-253/300-310`
  类型名出入改限定名；对象流写出的 `cc.name`（`VmExecutorSer.h:204-206` ↔ `:290-301`）同改，
  **不做旧格式兼容读**。
- 项目函数的键随之变化（今天根外/无 wrapper 的 `f` 是裸键，改后 `main.f`），由两处吸收：
  - **入口点**：`CompiledModule` 增 `entryPoint`（函数索引），`ModuleSaver/ModuleLoader` 落盘读回；
    `VmExecutor::Execute` 不再 `FindFunction("main")`（`VmExecutor.cpp:89`）——根目录的
    `main.n` 包名是 `main`，入口键变成 `main.main`，按名字找入口从此不可靠。
    顺带消灭「多个 TU 各有 `main()` 时首个胜出」的静默歧义（builder 检测重复入口 → 报错）。
  - **宿主 native**：`EnsureNativeAvailable` 先查限定名、回落裸名（`VmExecutorNativeHost.cpp:140-152`），
    使 `TestNatives.h:61-64` 与 e2e 宿主注册面不动。DLL 名规则留给阶段 5（那时库＝`.nlib`，
    dll 按库命名，符号按完整限定名）；今天那句「按第一个点切分」的实现在多段包下不成立，
    4e 用限定名优先＋裸名回落绕过，不扩写切分逻辑。
- `.nmod` 格式：`NMOD_FORMAT_MINOR` 12 → **13**（`CompiledModule.h:43`），floor 与 ceiling 同抬。
  4d 原定的 1.13 作废（4d 已排到阶段 5 之后，届时按当期号抬）。

### R3 `import` 到包级，带点的名字能落地

- 删除库发现的点分跳过（`ModuleBuilderImports.cpp:143/173-178`）：`import gfx.color;` 找
  `<搜索根>/gfx/color.n`。诊断文案同步纠正（今天会建议一条无法执行的指令）。
- 预编译 `.nmod` 在 4e 仍按「文件名＝导入名」定位（`:283`），即**点分导入名在 4e 只解析到源码库**；
  带点的名字落到 `.nmod` 上是阶段 5 `.nlib` 成员表的事。命中不到时诊断要说清是哪种形态没找到。
- 逻辑分组不单独开关可见性：导入一个包只给它自己，不给兄弟包（今天已是如此，wildcard 除外）。

### R4 语法：类型路径有自己的范畴，`namespace` 产生式删除

- 新增 `TypePath: IdentifierExpr | TypePath '.' TT_Identifier`（产 `SnQualifiedTypeExpr*`，段数由语法保证），
  `QualifiedType`（`nlang.y:1286-1297`）与 `HeadType` 都改走它；
  `HeadType: MemberExpr`（`:1324-1331`）与 `CollectQualifiedSegments`（`:158-172`）删除——
  `a.b() v;`、`a[0].x v;` 这类非法形状不再可构造，无 `Kind()` 检查的 `static_cast`（`:168-171`）
  和 `segs.at(0)/at(1)` 的 ≥2 段假设一起消失（4b-2 审核 Important 2）。
- 补 `TypePath '<' TypeList '>'`（`'<'` 今天只挂在 `:1259`/`:1311`/`:1432/1446`），
  `ClassInheritOpt: ':' NameExpr`（`:1056`）与 `Expression KT_As NameExpr`（`:1389`）改吃 `TypePath`。
- 删 `namespace` 产生式（`:551-553`）及其 AST 路径。冲突账本重测
  （现 1 rr ＋ 12 sr，`:1229-1245` 注释说明 `%expect` 不适用）。

### R5 连带面（一次改完，不留兼容层）

- `stdlib/{io,math,fs}.n`：去掉 `namespace` 外壳与缩进，名字由路径给出，公开拼写不变。
- 测试：内嵌源码里 11 处 `namespace`、`tests/test_vm/test_library_source.cpp`（现 17 例／18 CHECK）
  与 `test_module_import.cpp` 的 fixture、`tests/fixtures/native/*` 两个 fixture、e2e `use_mylib.n`／`thirdparty` 用例。
- `langservice::SymbolIndex`（`SymbolIndex.cpp:85-116` 按 `namespace` 抓 `ns` 的正则）改按路径。
- nide 补全/F12 的「库调用恰为 `ns.name`」假设（`CodeEditor.cpp:277/316-326/357-364`）校准；
  `ndb` 按名断点取首个同名（`MachineFrontEnd.cpp:135-145`）在 4e 记为已知不足（owner 化留后）。
  **IDE 可见行为变更需用户交互验证。**
- `.nproj` schema 里那个从没被 ncc 用过的 `namespace=` 属性（`ProjectFile.h:18-19` 自述「IDE-facing、忽略」）
  删除，免得它被当成第三个名字来源。
- 文档：`docs/user_manual/en`、`docs/user_manual/zh` 的 `namespace` 章节与 16 处提及改写为目录分组；公开文字不得残留旧引擎痕迹。

## 3. 决策记录

- **D1 名字唯一来源＝路径**（用户裁决 ③）。否决声明式 `package`（两个来源＋优先规则＋一致性检查）、
  否决「顶层 namespace 视同包」的特例折叠（靠相等才对得上，正是本阶段要移除的东西）。
- **D2 类型与函数共用一条名字缝**：`包名 + "." + 名`，成员方法恒裸名（分派约束）。
  `.nmod` 类型名与对象流名记录语义变更 ＋ 新增 `entryPoint` ⇒ 格式 12→13，floor/ceiling 同抬、不兼容读。
- **D3 限定符＝包名原文**，无末段简写、无 alias（§1.2）。多段包全面允许（源码库）。
- **D4 撞名报错不排序**：两个根给同一包名、两文件同名类型同包、重复入口——全部诊断，不 first-wins。
- **D5 `SnQualifiedTypeExpr` 保持「不可达 codegen」**，注释写明契约（R4 后非法形状不可构造）。
- **D6 语法专用范畴优先**：`Kind()` 守卫只作 LALR 冲突恶化时的回退；计划第一步就是纯语法可行性实验。

## 4. 阶段边界与顺序

- **4e 做**：R1～R5 全部；`.nmod` 12→13。
- **4e 不做**：每源文件一份 `.nmod`、`.nlib` 归档、链接器与未解析引用表（阶段 5）；
  `_package.n` 的目录级可见性与库元数据（4d）；
  成员/别名侧三处 owner 盲查找；`ndb` 断点 owner 化；IDE 交互验收（用户）。
- **建议的先后**：4d 挪到阶段 5 之后（未决项一）；4e 本身与此无关，现在就能开工。
- **提交节奏**：R4 语法可行性实验 → R1（路径名字＋容器一处＋撞名检查）→ R2（限定名字缝、
  类型表、延迟名表、`Import` 去重、`entryPoint`、格式 13）→ R3（点分 import）→
  R4（语法范畴＋删 `namespace`）→ R5（stdlib/测试/langservice/nide/文档）→ 文档页。
- **4b-2 审核闭环**：round 2 在 4e 落地后跑（`1bce551..4e-head`）。
- **阶段 5 的输入**（本稿只记录边界，设计另开）：AST 合并在产码前发生
  （`ModuleBuilder.cpp:233-258`）必须改成逐包产码；跨包引用要能先记名字后定址；
  可执行产物需要自己的名字（今天 `ncc run <module.nmod>`，`main.cpp:39`）；
  `.nproj` 里空转的 `intermediateDir`（obj 目录）是对象文件的落点。

## 5. 测试矩阵（真实编译＋执行，无 mock）

1. **同名类型隔离**（`rb3` 升级）：库 `alib/Point` ＋ 根 `Point` 布局不同，各自 `new`／字段读写／传参，断言数值。
2. **目录层级即分组**（rb8 P1/P2/P7）：`gfx/color/deep.n` 里的 `Shade` → `gfx.color.deep.Shade` 可拼可跑，
   类型与函数同权；同程序里 `import gfx.color;` 不给 `gfx.color.deep`。
3. **点分库名落地**（R3）：`-I <根>` ＋ `<根>/vendor/graphics.n` → `import vendor.graphics;` ＋
   `vendor.graphics.Sprite` 通过；`graphics.Sprite`（末段简写）与 `import graphics;` 必须失败。
4. **同末段两包共存**：`a/io.n` 与 `b/io.n` 同场，`a.io.f()`／`b.io.g()` 与两侧类型互不串，无歧义诊断。
5. **撞名报错**（D4）：两个 `-I` 根给出同一个包名 → 诊断；同包两个同名类型 → 诊断；
   两个 TU 各写 `main()` → 重复入口诊断。
6. **stdlib 形状迁移零回归**（R5）：`io.print`／`math.sqrt`／`fs.*` 现有公开拼写与输出逐字不变。
7. **限定身份进磁盘**：`.nmod` 的类型名与函数名为限定名，`entryPoint` 落盘读回；
   外部 `alib.Object` 不并入内置 `Object`，根级 `Object` 仍并入。
8. **宿主 native 回落**：裸名注册依旧可解析（现有 e2e native 面不回归）。
9. **泛型库类型**：`alib/vec.n` 的 `Vec` → `alib.Vec<int>` 声明＋使用，擦除键仍是 `"List"`。
10. **继承/cast 同权**：`class D : alib.B`、`x as alib.B`。
11. **流式字面量**：`readStruct("Node")` 唯一命中通过、两个可见 `Node` → 歧义诊断、零命中 → 现有诊断；
    同程序对象流往返仍绿。
12. **R4 语法负例**：`a.b() v;`、`a[0].x v;`、`a.b().c v;` 必须语法错误；`a.b c;` 合法；
    **`namespace x {}` 必须语法错误**（写不出来）。
13. **全量**：串行 ctest 63/63、`source_size_guard` 绿、docs gate 真 ncc/nvm 67 passed。
14. **负控**：至少一条「关掉 R2 的名字缝即失败」，用 CLI 探针验证，不改树内代码。

## 6. 风险

- **函数键变长影响面大于类型键**：`entryPoint` 与宿主 native 回落必须与 R2 同提交，
  否则 `ncc`/`nvm`/e2e 全线红（§5-7、§5-8 是锚）。
- **R5 是横切面**：stdlib、测试 fixture、langservice 正则、nide 补全、docs 一起动，
  范围最大、回归风险最高，单独提交，IDE 部分要用户交互验收。
- **R4 的 LALR 可行性未证明**：先纯语法实验再决定回退方案（删 `namespace` 会减冲突，
  新增 `TypePath` 会加冲突，净效果必须实测）。
- **对象流跨程序语义**：`cc.name` 由裸名变限定名（`VmExecutorSer.h:204-206/290-301`），
  D2 已裁决不保留兼容读，文档要写明。
- **4d 的重排**：`docs/dev/phase4d_plan.md` 的 v1.13 依赖清单方案在合并产物形态下已无意义，
  标注为「阶段 5 之后重写」。
