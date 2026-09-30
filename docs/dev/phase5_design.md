# 阶段 5 设计：包名与类型身份

> 状态：设计稿（docs/dev/，随分支跟踪；2026-09-30 自 temp/ 迁入）。分支 dev，行号基线 `d7ca710`。
> 阶段划分与共同模型见 `docs/dev/phases_567_design.md`（名字规则、`.nmod`/`.npack`/DLL 三层、
> 依赖方向在那里说一次，本稿不重复）。上游：`docs/dev/phase4b2_audit_notes.md` 第 1 轮
> Important 2/3/4。探针证据：`temp/rb6/RESULTS.md`（rb6/rb7/rb8，全部真实 ncc 运行）、
> `temp/rb3/`（同名类型串布局）、`temp/probe/`（bison 3.8.2 文法冲突实测）。
> 本稿取代 `docs/dev/phase4e_design.md`。
>
> 2026-09-29 复核：本文早先三处判断被实测推翻，已就地更正——容器不新建（§0.2）、
> 语法专用范畴不可行（R4/D7）、枚举类型身份不存在（§0.3-6）。

## 0. 本阶段修什么

### 0.1 根因两处

**第一处：名字的来源没有声明过，全靠巧合。**
`DeriveModulePath`（`src/compiler/builder/ModuleRegistry.cpp:48-65`）在项目根内给出相对根的
点分路径（`utils/helper.n` → `utils.helper`），根外或没有根时**退化成文件名**（`io.n` → `io`）。
库文件全走退化档，于是「一个库要能用」需要三样东西恰好相等：文件名＝文件里那层 `namespace`
名＝用户写的 `import` 名。stdlib 的 `stdlib/io.n` ＋ `namespace io`（`stdlib/io.n:5`、
`math.n:5`、`fs.n:5`）刚好满足，所以一直绿。不满足时该单元**两种拼写都不可达**
（rb6 B/C/D/E/F/G、rb8 P1/P2/P7），编译器还会建议一条无法执行的 `import gfx.color;`——
库发现明确跳过带点的名字（`src/compiler/ModuleBuilderImports.cpp:98-99` 「调用方只传裸名」
的注释、`:139-144` 的函数头注释、`:178-179` 的 `continue`）。

**第二处：VM 里函数用带前缀的名、类型一律用裸名。**
`QualifiedFunctionName`（`src/vm/backend/Register.cpp:24-41`）只给「祖先里有 `NK_Namespace`」
的函数加前缀；`Register.cpp:47`、`RegisterClass.cpp:95/100` 写类型表时一律用 `sn.Name()`；
查找 `include/nlang/vm/CompiledModule.h:314-326` 首个匹配、零重复检测。
`alib.Point` 与根 `Point` 同时在场 → 代码拿到先注册者的字段布局，无诊断（`temp/rb3/` 已复现）。

### 0.2 两处根因是同一处

今天函数前缀**只在库文件写得出**（因为容器节点来自用户写的 `namespace`），项目模块的成员
直接挂在 root 上——`CompiledInFunctions`（`ModuleRegistry.cpp:293-300`）与 `FindModuleType`
（`:377-385`）都是这个规则：`isLibrary` 才去找 `namespace <path>` 容器，否则回落到 root。
所以 `utils/helper.n` 里的 `help()` 在 VM 表里的键是裸的 `help`，与 `core/helper.n` 里的
`help()` 同键。类型更糟：连前缀都没有。

本阶段的正面动作只有一件：**包名从节点的所有者标签算，不新建容器**。每个根级成员在合并阶段
已被打上所属模块索引（`ModuleBuilder.cpp:262-271` `TagUnitMembers`，产码前仍然有效），
`ModuleRegistry::ModulePathOf`（`ModuleRegistry.cpp:170-175`）已经把索引映成点分路径，
两者拼起来就是包名——项目模块与库文件同一条规则，`isLibrary ? FindField(path) : root`
的容器判定从此没有存在必要。

早先设计的「编译器按路径建容器、成员挂进去」**已撤回**：新建 AST 容器会连带牵出裸名池
可见性（`ExprResolverMemberFields.cpp:45-48` `IsBarePoolScope`）、同目录自动可见
（`ModuleRegistryGate.cpp:129-166` `BuildGate`）、`AllowMember` 的全局命名空间内建类型
豁免（`SnMisc.cpp:145-159`）、wildcard/using 的落点四组语义，收益只是让查找少绕一次 owner
表。所有者标签本来就在，路径本来就算好了。

### 0.3 相对原 4e 稿的四处的更正

1. 原稿把「项目函数键变长」写成附带影响；实际上它是本阶段最大的一面：项目侧**所有**自由
   函数与类型的键都从裸名变成限定名（§0.2）。原稿的「宿主 native 先查限定名、回落裸名」
   随之作废——回落是一层兼容垫片，改为测试注册表直接写限定名（⑥ 不留兼容层）。
2. `native` 声明的位置限制改由**编译期诊断**给出：所在包必须单段
   （DLL 是平面单位、`EnsureNativeAvailable` 按第一个点决定模块名，
   `VmExecutorNativeHost.cpp:146-152`）。今天这条在多段包下是运行期才炸的错，
   阶段 6 库身份出现后解除该限制。
3. `.nlib` 全部改叫 `.npack`（⑤）。
4. 格式号 12→13 无迁移成本：仓库里**没有已提交的 `.nmod`**（`git ls-files '*.nmod'` = 0），
   二进制产物全部由测试现场生成。
5. **枚举没有类型身份可做**：VM 侧只有 `enumNames`（`CompiledModule.h:305`，存值名、按序号
   使用），`CompiledModule` 的四个线性查找里没有 `FindEnum`（`:307-334`）。原稿的「枚举
   表改限定名」删除——没有名字键控的枚举表可改。
6. **冲突账本注释是过期的**：`nlang.y:1229-1245` 写「1 rr ＋ 12 sr ＝ 13」，实测
   （bison 3.8.2，`temp/probe/base.y`）是 **0 rr ＋ 14 sr**，分布为 state 148/185/191 各 1、
   254 共 9、290 共 2——`'<'` 的那条 reduce/reduce 已不再是 rr。账本要按实测重写，
   否则「计数变化即未授权改动」这条漂移信号本身就是错的。

## 1. 规则（每条一处实现）

### R1 名字

- `PackageOf(node)` ＝ `ModulePathOf(OwnerOf(node))`，所有者缺失（内建类型、合成泛型实例、
  未打标签的节点）时为空串。一处产出，函数与类型同用。
- `ModulePathOf` 的两处派生要修：**根外语义**。项目模式下
  `DeriveModulePath`（`ModuleRegistry.cpp:48-65`）对根外文件退化成文件名——库文件的包名
  必须由**它命中的那个搜索根**相对算出（`-I r` ＋ `r/gfx/color.n` → `gfx.color`），
  所以 `ModuleBuilderImports.cpp:100-111 FindLibrarySourceFile` 要把匹配到的根交回注册侧；
  单文件编译没有根（`ncc/main.cpp:279-287` 只在项目模式设 `m_sProjectDir`），起点＝该文件
  自己所在目录，即包名退化成 stem 是**规定**而非巧合，且此时不再有第二条拼写。
- 取代 `CompiledInFunctions`（`ModuleRegistry.cpp:291-300`）与 `FindModuleType`
  （`:375-385`）里的 `isLibrary ? FindField(path) : root` 分支：两处都只剩「root 上
  `equal_range` ＋ owner 过滤」。另外三处 owner 盲的查找收拢到同一条规则：
  `Access(SnIdentifierExpr)` 的类型分支（`ExprResolverTypes.cpp:425-433`）、
  `ResolveClassBases`（`StatementResolverTypes.cpp:27-56`）、
  流式类型实参链走（`ExprResolverMemberBuiltins.cpp:306-310`）。共 5 处。
  **不动**：`ProbeNonFunctionField`、`FindFieldInUsings`、`CheckUnitAliases`
  （成员／别名可见性，与类型身份无关）。
  **轮 1 计划审核对本段的实测修正**（写计划时才发现行号和「5 处」都不准）：
  ① `ExprResolverTypes.cpp:425-433` 是 `Access(SnIdentifierExpr)` 里的**函数**裸池过滤器
  （`bareFuncFilter`／`IsBareVisible`，已经带 owner 判定），不是类型分支；类型候选走的是
  `:432` 的 `FindFieldInAncestor`，它对类型不做 owner 过滤（first-wins）。
  ② `ResolveClassBases` 自己不做查表，它 `Resolve(*sn.SuperName())` 委托给同一个
  `Access(SnIdentifierExpr)` → 修了①就顺带修了它，**不是独立的一处**。
  ③ `CompiledInFunctions` 与 `FindModuleType` 必须与去壳同提交（壳里的成员不在 root 字典上）。
  ④ 流式字面量那一处即 D10，已单列。
  所以「5 处」实际是 **4 个改动点、3 个提交**：Task 5 Step 3b（两处容器判定）、
  Task 4 Step 9（D10）、以及**尚未排期的 ①**（`FindFieldInAncestor` 的类型候选 owner 过滤）。
  ① 的影响面：导入了含 `Point` 的库之后，本 TU 写裸 `Point q;` 会绑到库里那个而不是本文件的。
  它是否留在阶段 5 做，**待用户裁决**（做＝再加一个改动点＋负例测试；不做＝记为阶段 7 的已知缺口）。
- `pRoot->FindField(path)` 今天是单段字典命中（`src/compiler/SnMisc.cpp:34-37`，
  `SnFunctionParentField::FindField` 无递归）。owner 化之后带点路径不再需要逐段下降到
  容器：路径整体就是注册表里的 `m_modules[i].path`，比较路径即可。
- 撞名一律诊断、不排序、不 first-wins：两个 `-I` 根给出同一个包名（今天
  `Import.cpp:237/243/248` 静默先到先得）；同包两个同名类型；多个 TU 各有 `main()`。
  这条检查同时取代保留名表 `IsReservedLibraryName`（`ModuleRegistry.cpp:78-98`）——
  撞 `io`/`math`/`fs` 有真检查后，专门的表是多余的第二套规则。
- 入口点：`CompiledModule` 增 `entryPoint`（函数索引），`ModuleSaver`/`ModuleLoader` 落盘读回；
  `VmExecutor::Execute` 不再 `FindFunction("main")`（`src/vm/VmExecutor.cpp:89`）——根目录的
  `main.n` 包名是 `main`，入口键从此是 `main.main`，按名字找入口不可靠。
  阶段 6 这个字段搬到 `.npack` 头部（`docs/dev/phases_567_design.md` §2 阶段 6 第 5 条）。

### R2 类型与函数共用一条名字缝

- 缝落在**编译器侧**：新函数 `PackageOf(const SyntaxNode&)`／`QualifiedName(const SyntaxNode&)`
  读 `ModuleRegistry` 的 owner 表与路径表（解析器有 `m_Env.Registry()`；D10 的字面量改写也在
  这一侧）。`VmBackend` 今天**拿不到注册表**（`ModuleBuilder.cpp:176-197` 只注入 imported
  modules 与 library index），按同一先例补一个 `SetModuleRegistry(const ModuleRegistry*)`
  在 `GenerateCodes` 里注入，产码侧的 `Register.cpp` 私有的 `QualifiedFunctionName()`
  （`:24-41`，靠祖先里的 `NK_Namespace` 拼前缀）改为调用这条缝。**两侧共用一个函数是
  必须的**：否则 `readStruct` 写出的串与表键会各自漂移。
  **保留**成员方法的裸名分支（`:25-29` 的 Class/Interface/Enum 父节点判定）——方法按接收者
  裸名分派，加前缀会让每次查找都失败。
- **构建面**：`ModuleRegistry.h` 是编译器私有头（`src/compiler/builder/`，公开头树
  `include/nlang/compiler/` 是平铺的、没有 `builder/` 子目录），`nlang_vm` 的 include 路径
  （PUBLIC `include/` ＋ PRIVATE `src/vm`）看不到它。给 `src/vm/CMakeLists.txt` 的
  `nlang_vm` 加 PRIVATE `${PROJECT_SOURCE_DIR}/src/compiler`，反向先例是
  `src/compiler/CMakeLists.txt:99` 把 `src/vm` 列为 PRIVATE。链接不是问题：
  `BUILD_SHARED_LIBS` 不在 cache（静态默认），`ModuleRegistry` 无导出宏也可用。
- `CompiledStruct::name`/`CompiledClass::name` 存限定名。天然不变的四类：
  泛型擦除基名（`BaseName()` 的 `"List"`/`"Dict"`，`SnMisc.h:307-309`、`EmitExprNew.cpp:55`、
  `EmitExprInitList.cpp:305-307`）；内建类型与异常名；成员方法（裸名分支）；
  字符串字面量型类型名（`readStruct("Node")` 一族，见 D-stream）。前三类在 owner 化之后
  不需要特例判断——合成 decl 与内建 decl 没有所有者标签，`PackageOf` 返回空串就是裸名。
- 24 个读取点分两类：AST 在手 → `QualifiedName(*decl)`；字面量／擦除名 → 原样。
  延迟字段类型名表（`Register.cpp:63/86/101`、`RegisterClass.cpp:113/117/161`）的
  写入端与解析端**同提交**改；`declMap`（`RegisterClass.cpp:93/100/157`）改限定键。
- 注册时同键重复 → 诊断（今天 `Register.cpp:47`、`RegisterClass.cpp:95` 无条件 push_back；
  `RegisterClass.cpp:157-158` 的 `declMap.find(cc.name)` 失败即静默 `continue`）。
- `Import.cpp:237/248` 的去重改按限定名：外部 `alib.Object` 不再并入内置 `Object`
  （今天 `:241` 的注释把这当特性）。原稿「用户根级 `Object` 仍并入内置 `Object`」
  **删**：限定之后用户 `main.Object` 与内置 `Object` 是两个键，不再是同一名。
- 磁盘与序列化跟着变：`ModuleSaver.cpp:178-180/223-225` ↔ `ModuleLoader.cpp:245-253/300-310`
  类型名出入改限定名；对象流写出的 `cc.name`（`VmExecutorSer.h:204-206` ↔ `:290-301`）同改，
  **不做旧格式兼容读**。
- 宿主 native：DLL 侧键规则不变（`nlang_<包名>.dll` 注册 `<包名>.<符号>`，
  `VmExecutorNativeHost.cpp:148-152`），stdlib 三段单段包不受影响。测试侧宿主注册改限定名：
  `TestNatives.h:61-64`、`test_native_host.cpp:77-84`。**不做裸名回落。**
- `.nmod` 格式：`NMOD_FORMAT_MINOR` 12 → **13**（`CompiledModule.h:43`），floor 与 ceiling 同抬。

### R3 `import` 到包级，带点的名字能落地

- 删除库发现的点分跳过（`ModuleBuilderImports.cpp:178-179`）：`import gfx.color;`
  找 `<搜索根>/gfx/color.n`。诊断文案同步纠正（今天会建议一条无法执行的指令）。
- 预编译二进制在本阶段仍按「文件名＝导入名」定位（`FindModuleFile`，`:279-289`），即
  **带点导入名只解析到源码库**（`docs/dev/phases_567_design.md` §3 待决 2）。命中不到时诊断
  要说清找的是源码还是二进制。
- 导入一个包只给它自己，不给兄弟包（今天已是如此，wildcard 除外）。目录级对外可见性是
  阶段 7 的 `_package.n`，本阶段不动。

### R4 语法：`namespace` 产生式删除，限定类型名保持「链在表达式里、归约时才定身份」

**专用范畴已被实测否决**（`temp/probe/`，bison 3.8.2，只数冲突不产码）：

| 变体 | shift/reduce | reduce/reduce | 新增冲突 |
|---|---|---|---|
| `base.y`（现状） | 14 | 0 | — |
| `a.y`（加 `TypePath` 范畴，`Type`/`HeadType`/继承／`as` 全改吃它） | 16 | 0 | 2 条，全在 `'.'`，**危险** |
| `b.y`（删 `namespace` 产生式） | 14 | 0 | 无 |
| `ab.y`（两者叠加） | 16 | 0 | 同 `a.y` |
| `d.y`（删 `namespace` ＋ 继承／`as` 复用现成的 `NameExpr` 引导 `QualifiedType`） | 16 | 0 | 2 条，全在 `'.'`，**良性** |

`a.y` 的两条（`a.output` state 53／93）：`TypePath: TT_Identifier • '.' TT_Identifier` 与
`IdentifierExpr: TT_Identifier •` 在 `'.'` 上冲突，bison 默认取 shift ⇒ 看到 `p . x` 就永久
进入类型路径，成员访问的表达式读法被丢弃。`p.x = 1;`、`obj.field` 在第一遍运行即报错。
这不是可以「用 `%expect` 钉住」的噪声，是语言被改坏——**语句头不能有专用带点范畴**。
`import a.b.c` 之所以能用同样的形状（`ImportPath: TT_Identifier | ImportPath '.' TT_Identifier`，
`nlang.y:495-504`），是因为前面有 `KT_Import` 关键字消歧。

`d.y` 的两条（state 72／73）形状是 `TypeName: NameExpr •` / `QualifiedType •` 对
`QualifiedType: • '.' TT_Identifier`，同样默认 shift；但这两态只从 `':'` 与 `as` 到达
（态里没有 `Type`/`Expression` 的项，说明没有和表达式上下文合并），而**没有任何产生式期望
类型名后面跟 `.`**，shift 就是唯一正确读法。它们与现有 14 条同属「默认解即意图」的家族，
可以接受。

因此本阶段的做法是把已有的「归约时才定身份」形状**补上检查**而不是换掉：
- `CollectQualifiedSegments`（`nlang.y:160-172`）返回 `bool`：只有 `NK_IdentifierExpr` 与
  「inner 是 `IdentifierExpr` 的 `NK_MemberExpr`」递归可接受，遇到 `InvokeExpr`、
  下标、括号等 outer/inner 形状返回 false 并停止收集。
- `HeadType: MemberExpr` 的动作（`:1324-1332`）不再无条件 `static_cast`，失败时产一个
  标记为非法的 `SnQualifiedTypeExpr`；`Access(SnQualifiedTypeExpr&)`
  （`ExprResolverTypes.cpp:327-340`）现有的 `segs.size() < 2` 内部错误文案换成
  面向用户的诊断（「qualified type name must be a dotted identifier chain」）。
  语法从不报语义错（`nlang.y` 全文无 `yyerror`），诊断留在 resolver 是既有纪律。
- `segs.at(0)/at(1)` 的 ≥2 段假设（`:1327`）由返回 `false` 的路径消除。
- `ClassInheritOpt: ':' NameExpr`（`:1056`）与 `Expression KT_As NameExpr`（`:1389`）改吃
  `TypeName: NameExpr | QualifiedType`（即 `d.y` 的形状）：跨包基类与跨包 `as` 因此可用，
  代价是上表的 2 条良性 `'.'` 冲突，账本一并记下。
- **不补** `QualifiedType '<' TypeList '>'`：用户自定义泛型类不存在，`GetGenericClassDecl`
  （`ExprResolverTypes.cpp:103-152`）只认内建 `List`/`Dict`/`Func`，所以
  `alib.Vec<int>` 的正确结果是诊断而不是语法。4b-2 审核 Important 4 里那条
  「`alib.Vec<int>` 无处可解析」按「不支持，给诊断」处理。
- 删 `namespace` 产生式（`:553-555`）与 `NamespaceMember: Namespace`（`:534-536`）：实测零冲突代价。
  `NK_Namespace` 节点类别保留（root 自身就是它，`TranslationUnit.cpp:30`；导入 stub 的
  容器由 `ImportedNodeBuilder.hpp:67` 建）。
- 冲突账本注释（`:1229-1245`）按 §0.3-6 的实测值重写。

### R5 连带面（一次改完，不留兼容层）

- `stdlib/{io,math,fs}.n`：去掉 `namespace` 外壳与缩进，名字由路径给出，公开拼写不变。
- 测试：内嵌 fixture 源码里 11 处 `namespace`（`test_module_import.cpp` 10、
  `test_library_source.cpp` 10，另 `test_symbol_index.cpp`/`test_searchpath_integration.cpp`/
  `test_thirdparty.cpp` 各 1）＋ `tests/fixtures/native/{mylib,mixlib}.n` ＋
  e2e `use_mylib.n`／`thirdparty` 用例 ＋ 宿主 native 注册名（R2）。
- `langservice::SymbolIndex`（`SymbolIndex.cpp:85-116` 按 `namespace` 抓 `ns` 的正则）改按路径。
- nide 补全/F12 的「库调用恰为 `ns.name`」假设（`CodeEditor.cpp:277/316-326/357-364`）校准；
  `ndb` 按名断点取首个同名（`MachineFrontEnd.cpp:135-145`）本阶段记为已知不足（owner 化留后）。
  **IDE 可见行为变更需用户交互验证。**
- `.nproj` schema 里从没被 ncc 用过的 `namespace=` 属性删除（`ProjectFile.h:12/18` 自述
  「IDE-facing、忽略」），免得它被当成第三个名字来源。
- 文档：`docs/user_manual/en`、`docs/user_manual/zh` 的 `namespace` 章节与 16 处提及改写为目录分组；
  公开文字不得残留旧引擎痕迹。

## 2. 决策记录

- **D1 名字唯一来源＝路径**（①）。否决声明式 `package`（两个来源＋优先规则＋一致性检查）、
  否决「顶层 `namespace` 视同包」的特例折叠（靠相等才对得上，正是本阶段要移除的东西）。
- **D2 类型与函数共用一条名字缝**：`包名 + "." + 名`，成员方法恒裸名（分派约束）。
  类型名入盘＋对象流名记录语义变更＋新增 `entryPoint` ⇒ 格式 12→13，floor/ceiling 同抬、
  不兼容读。
- **D3 限定符＝包名原文**，无末段简写、无 alias：末段天然重复（`utils/helper.n` 与
  `core/helper.n` 都是 `helper`），简写就要新增冲突诊断，出路只剩 alias 语法。取原文后
  源码里写的、VM 表里存的、报错里显示的永远是同一个串。
- **D4 撞名报错不排序**（R1 第 4 条）。
- **D5 `native` 所在包必须单段**，编译期诊断，阶段 6 解除（§0.3-2）。
- **D6 `SnQualifiedTypeExpr` 保持「不可达 codegen」**，注释写明契约（非法链在 resolver 就被拒）。
- **D7 语法专用范畴已否**（原稿把它列为首选、`Kind()` 守卫作回退；`temp/probe/a.y` 实测
  表明该形状在语句头抢走成员访问的 `'.'`，回退成为唯一方案）。守卫不是临时补丁：它是
  「链在表达式里、归约时才定身份」这一 LALR 约束的**正面实现**，`d.y` 证明同样的思路用在
  `':'`／`as` 位置可以扩。
- **D8 包名取自所有者标签，不新建 AST 容器**（§0.2）。
- **D9 跨包泛型实参拼写不支持**，给诊断（R4）。**轮 3 实测降级（2026-09-29，`build-dev/tests/Release/ncc.exe`）**：
  `alib.Vec<int> v;` 今天得到的是裸 `syntax error`——泛型实参只挂在 `IdentifierExpr` 上，限定链根本进不了
  那条产生式。本阶段只做「不误接受」的负例钉（期望串就是 `syntax error`）；**指名字符串「用户类型不支持泛型
  实参」不在本阶段做**，它需要一条能吃下 `QualifiedType '<' ... '>'` 的 `error` 恢复产生式，为一句文案动语法
  不划算 ⇒ 记为阶段 7 的缺口。
- **D10 字面量类型名在编译期定身**：`readStruct("Node")`／`writeObject` 一族传的是字符串，
  运行期按名查表（`ExprResolverMemberBuiltins.cpp:286-310` 的解析链对 owner 是盲的）。
  规则：编译期把字面量按**调用点可见作用域**解析成声明，改写成canonical限定名再产码；
  零命中沿用现有诊断，多命中报歧义，**不留运行期裸名回落**。这样 7 个 e2e `.n` 源文件
  一字不改，而表键限定化之后仍然命中。

- **D11 名字缝在编译器侧、VM 侧注入注册表共用**（R2 第一条）。
- **D12 顺序约束：限定化必须先于 `namespace` 删除落地**。只要祖先链走法还在，删掉
  `stdlib/io.n` 的外壳会让 `io.print` 变成裸 `print`，DLL 注册表（`nlang_<包名>.dll` ＋
  `<包名>.<符号>`）当场对不上。owner 化之后包名来自路径，而 stdlib 文件的
  `DeriveModulePath` 退化值恰好是 `io`/`math`/`fs`（stem＝文件名＝今天的 `namespace` 名），
  所以「换缝」这一步不动任何键——这是它能在同一次提交里全绿的真正原因。

- **D13 表键即展示名，不另设第二套短名**。字节码函数表的键同时是人眼看到的名字：
  回溯（`VmExecutor.cpp` `FormatBacktrace`）、活动帧（`ndb`／IDE 的 frame 字段）、
  断点按名命中（`test_debug_client.cpp:162/178`、`test_mainwindow.cpp:2284/2556`）。
  限定化之后这些字符串一律变成 `包名.符号名`，即 `main` 在终端里显示为 `at main.main`。
  备选方案是展示时剥掉最后一段之前的前缀，但那要求表键与展示名两套规则长期同步，
  一旦新增消费面（`ndisasm`、coredump、日志）就会分叉。**决定：不加第二种名字**，
  接受 `main.main` 这类可见变化，并同步改约 10 处测试断言。
  **这条改的是人在终端／IDE 里看到的字符串，不是内部键，所以需要用户点头。**

## 3. 边界与提交顺序

- **做**：R1～R5，`.nmod` 12→13。
- **不做**：每源文件一份 `.nmod`、`.npack`、链接器与未解析引用表（阶段 6）；依赖记录、
  过期检测、`_package.n`、`ndb` 库源码、`runNccBuild` 异步化（阶段 7）；
  成员／别名侧三处 owner 盲查找；`ndb` 断点 owner 化；IDE 交互验收（用户）。
- **提交节奏**（每一步单独可测，串行 ctest 63/63 保持绿）：
  1. 语法收口（不碰 `namespace`）：账本注释按实测重写（0 rr ＋ 14 sr）→
     `CollectQualifiedSegments` 带检查 → 非法形状走 resolver 诊断 → `':'`／`as` 吃
     `TypeName`（`d.y` 形状，＋2 良性冲突一起记账）。收掉 4b-2 审核 Important 2。
  2. 名字缝（R1＋R2 必须同提交闭合）：`PackageOf`/`QualifiedName` ＋
     `VmBackend::SetModuleRegistry` ＋ 24 个读点 ＋ `declMap`／延迟名表 ＋ 重复键诊断 ＋
     `Import` 限定去重 ＋ `entryPoint` ＋ 格式 12→13 ＋ 宿主注册名 ＋ D10 字面量改写。
     这一步之后 stdlib 的键**一字不变**（D12 的巧合此刻仍然成立：owner 路径＝stem＝外壳名），
     项目侧键全部变长——§4-3/§4-8 是锚。
  3. 删 `namespace`：产生式＋词法＋`stdlib/{io,math,fs}.n` 外壳＋内嵌 fixture＋
     `tests/fixtures/native/*.n` 一起。缝已是 owner 基，删外壳不再移动任何键。
  4. R3 带点 import 落地：库搜索根参与包名、点分名找 `<根>/a/b/c.n`、保留名表退役改撞名诊断。
  5. R5 连带面：langservice 正则、nide 补全假设、`.nproj namespace=` 属性删除。
     IDE 部分要用户交互验收。
  6. 文档页（en＋zh 双导航）＋ CHANGELOG。
- **审核闭环**：4b-2 round 2 在本阶段落地后跑（`1bce551..阶段5-head`），随后本阶段自己
  跑新鲜评审轮直到一轮无新增 Critical/Important。

## 4. 测试矩阵（真实编译＋执行，无 mock）

1. **同名类型隔离**（`rb3` 升级）：库 `alib/Point` ＋根 `Point` 布局不同，各自 `new`／字段
   读写／传参，断言数值。
2. **同末段两包共存**：`a/io.n` 与 `b/io.n` 同场，`a.io.f()`／`b.io.g()` 与两侧类型互不串。
3. **项目模块也是限定身份**（§0.2 的正面对手戏）：项目里 `utils/helper.n` 与 `core/helper.n`
   各有一个 `help()` 与同名 `Cfg`，断言互不串。
4. **目录层级即分组**（rb8 P1/P2/P7）：`gfx/color/deep.n` 里的 `Shade` →
   `gfx.color.deep.Shade` 可拼可跑，类型与函数同权；`import gfx.color;` 不给 `gfx.color.deep`。
5. **带点库名落地**（R3）：`-I <根>` ＋ `<根>/vendor/graphics.n` → `import vendor.graphics;`
   ＋`vendor.graphics.Sprite` 通过；末段简写 `graphics.Sprite` 与 `import graphics;` 必须失败。
6. **撞名报错**（D4）：两个 `-I` 根给出同一个包名；同包两个同名类型；两个 TU 各写 `main()`
   ——三条都是诊断且信息指名道姓。
7. **stdlib 形状迁移零回归**（R5）：`io.print`／`math.sqrt`／`fs.*` 现有公开拼写与输出逐字不变。
8. **限定身份进磁盘**：`.nmod` 里类型名与函数名为限定名，`entryPoint` 落盘读回；
   外部 `alib.Object` 与内置 `Object` 是两个键（用户根级 `Object` 亦然，键为 `main.Object`）。
   **本阶段钉到键为止**（轮 4 实测）：今天根上写 `class Object { int marker; }`，类表里
   同时出现 `[0] Object (fields=0)`（内置）与 `[11] Object (fields=1)`（用户）两个同名键，
   而 `new Object()` 编到内置那份，运行报 `struct field store out of bounds`；写方法则
   编译期就失败。根因是编译器侧按**裸名**短路到内建单例的 5 个点（`ExprResolverNew.cpp:120`、
   `ExprResolverTypes.cpp:393`、`ExprResolverValues.cpp:46`、`ExprResolverStdLib.cpp:59`、
   `DuplicateFieldChecker.hpp:279`）——**未排期，待用户裁决是本阶段做还是记阶段 7 的账**。
9. **native 面**：stdlib 三段 DLL 解析不回归；宿主限定名注册正例；多段包内写 `native`
   → 编译期诊断（D5）。
10. **泛型库类型**：`alib/vec.n` 的 `Vec` 声明＋`alib.Vec` 使用通过；`alib.Vec<int>`
    → 实测今天的裸 `syntax error`（D9 轮 3 降级：本阶段只钉「不误接受」，指名字符串留到阶段 7），
    擦除键 `"List"` 不受影响。
11. **继承／cast 同权**（R4）：`class D : alib.B`、`x as alib.B` 通过；
    `class D : a.B.C`（三段）通过或给出指名诊断——`':'`／`as` 走的是同一个 `TypeName`。
12. **流式字面量**：`readStruct("Node")` 唯一命中产码后表键为限定名、往返仍绿；两个可见
    `Node` → 歧义诊断；零命中 → 现有诊断；**歧义诊断让用户写 `'pkg.Node'`，所以带点字面量
    本身也必须能用**（轮 4 实测：现在收集面是调用者的 AST 作用域链，`import` 不往链上加
    节点，库里的类型 `readStruct("S")` 今天直接报 `type not found`——这条不是「换判据」，
    是换信息源）。
13. **语法负例**：`a.b() v;`、`a[0].x v;`、`a.b().c v;` → resolver 诊断（不是崩溃、不是
    静默）；`a.b c;` 合法；**`namespace x {}` 语法错误**（写不出来）。
    **轮 4 实测：这三条今天不是「诊断不对」，是根本没有诊断**——同一形状跑 5 次给出
    `bad allocation`／脏模块名／**访问违例（rc=139）**三种结果，属于活体 UB。所以本条的
    实现工作是「把 UB 变成确定诊断」，负例钉子必须走**子进程**门（崩了只砸一条 manifest
    条目，不带走整个测试二进制）。
14. **全量**：串行 ctest 63/63、`source_size_guard` 绿、docs gate 真 ncc/nvm 67 passed
    （`NLANG_NCC`/`NLANG_NVM` 未设会静默跳 5 条）。**注意 `tests/e2e/manifest.txt` 不在
    ctest 里**（ctest 侧只有 `e2e_compile`，它只编 `examples/hello.n`），所以任何碰
    `tests/e2e/**` 的改动都要另外跑 `run_e2e_tests.py`，光看 63/63 不算过门。
15. **负控**：至少一条「关掉 R2 的名字缝即失败」，用 CLI 探针验证，**不改树内代码**；
    若确实要临时改树，必须在**该任务提交完成之后**做，改完立刻 `git checkout HEAD -- <文件>`
    并用空的 `git status --porcelain` 证明树干净——轮 4 之前这条没写时序，负控跑在提交前
    会把同文件的其他改动一起冲掉。

## 5. 风险

- **最大面是项目侧键全部变长**（§0.2）。R1 与 R2 必须闭合在同一提交里；`entryPoint` 与
  宿主注册名一起，否则 `ncc`/`nvm`/e2e 全线红（§4-8、§4-9 是锚）。
- **R5 是横切面**：stdlib、测试 fixture、langservice 正则、nide 补全、docs 一起动，
  范围最大、回归风险最高，单独提交，IDE 部分要用户交互验收。
- **R4 的 LALR 已实测**（`temp/probe/`）：语句头专用范畴不可行，形状保留、检查补齐。
  残余风险是 `':'`／`as` 那 2 条新 `'.'` 冲突的默认解——上表已论证「没有产生式期望类型名
  后跟 `.`」，§4-11 是它的实测锚。
- **对象流跨程序语义**：`cc.name` 由裸名变限定名（`VmExecutorSer.h:204-206/290-301`），
  D2 已裁决不保留兼容读，文档要写明。
- **阶段 6 的输入**（本稿只记边界）：AST 合并在产码前发生（`ModuleBuilder.cpp:233-258`）
  要改成逐包产码；跨包引用要能先记名字后定址；`entryPoint` 搬到 `.npack` 头部；
  `.nproj` 里空转的 `intermediateDir` 是对象文件落点；`native` 的单段限制在此解除。
