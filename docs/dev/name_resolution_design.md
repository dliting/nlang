# 名字解析与命名冲突统一设计

版本：v1.0（2026-11-04）。属v0.12.7批次，与数组/张量统一（array_tensor_design.md）并行。

## 1 背景

用户指令链（2026-11-04，四条，全部有约束力）：

1. 「非裸名命名冲突也要有完善解决方案，因为package可能嵌套。例如"my.foo.bar.x()"和"foo.bar.x()"，在前者"import my.*"后，和后者可能冲突。」
2. 「必要的话，补充到语言规范文档中。」
3. 「合理性优先，原有实现不合理的应该重构改进。」
4. 「superpowers，先设计再实现，因为这是比较复杂的问题。」

触发场景：嵌套包下限定名（非裸名）是否可能歧义；连带审计发现裸名可见性模型存在以调用方目录位置为转移的不一致行为。

## 2 探针事实矩阵

全部为ncc实测（temp/nestclash，工程含main.n＋my/foo/bar.n＋foo/bar.n两个同名异位模块，nproj形态）：

| # | 场景 | 结果 |
|---|------|------|
| Q1 | `import my.*;`＋`my.foo.bar.x()` | 通过。绝对路径限定调用可达 |
| Q2 | `import my.*; import foo.bar;`＋`foo.bar.x()` | 绑定顶层foo.bar（返回2），绝非my.foo.bar。通配不产生`foo`→`my.foo`别名 |
| Q3 | `import my.*; import foo.bar;`＋裸`x()`（两模块同形`int x()`） | 拒绝，报"not visible here...lives in 'my.foo.bar'"——指向未导入方、建议无效 |
| Q4 | 仅`import my.*;`＋裸`x()` | 拒绝（通配不授裸名） |
| Q5 | 仅`import foo.bar;`＋裸`x()` | 拒绝。显式导入不给嵌套包裸名可见性 |
| Q6 | Q5但两模块异形（`x()`/`x(int)`） | 同样拒绝。与形状无关 |
| Q7 | 无任何import＋裸`print`/`full` | 拒绝（TU未内联，名字不存在） |
| Q8 | 子目录TU（dir="sub"）＋`import io;`＋裸`print` | **拒绝**——同一条import语句能否裸用取决于main.n所在目录 |
| Q10 | 同目录兄弟文件，**无import**，裸`add`与限定`helper.add` | 双双通过。D7同目录隐式互见实证 |
| 既有 | 根TU（dir=""）＋`import tensor;`＋裸`full` | 通过——扁平库路径目录部为`""`，与根TU同目录 |

机制定位（代码事实）：

- 模块路径＝相对包根目录点化（DeriveModulePath）；重复路径在注册期拒绝（RejectsModulePath）；限定调用＝整链拼接后按精确键查注册表（TryResolveModuleCallTarget→IsKnownModule），**构造上唯一**。
- 裸名池过滤IsBareVisible＝无属主∨本模块∨同目录（ShareBarePool比较模块路径目录部）。库TU路径扁平（"io"无点）→目录部`""`→与根TU同池：**根TU裸用库函数是目录巧合，不是规则**。
- 导入门（ImportGate.exact/wildcards）只用于限定调用准入（RejectUnimportedModuleCall），裸名可见性完全不查门。

## 3 主流方案锚定

| 语言 | 显式import | 通配/按需 | 撞名处理 |
|------|-----------|----------|---------|
| Java（JLS §7.5.1/§7.5.2、§6.5.5.1） | 单类型导入→简单名直接用 | 按需导入也给简单名；**不递归子包** | 同名两个单类型导入＝编译错误；按需歧义＝使用点错误；单导入遮蔽按需与本包声明 |
| C# | using→不限名使用 | 同 | 歧义＝CS0104，点名双方 |
| Rust | `use path::item`→裸用（显式性优先的社区文化） | glob `use m::*`给裸名但**低优先级**（本地/显式名胜出）；官方与社区明诫慎用（prelude/测试场景除外） | E0659使用点歧义错误，点名候选，修法＝完整路径；两个显式同名导入＝使用点错误，修法＝as别名 |
| TypeScript/ES模块 | 命名导入`import {x}`→裸用 | **`import * as ns`恒为命名空间限定**；通配裸倾倒被ES模块规范明确拒绝（模块变更即引入不可控冲突） | TS2300重复标识符；别名`as`消解 |
| Swift | `import Module`→模块全体成员裸用 | 无通配（模块粒度） | 歧义诊断＋消歧语法 |
| Kotlin | import→裸用 | `import x.*`裸用 | 歧义错误 |
| Go/Zig | **限定专用**（`import "fmt"`→`fmt.X`）；无裸导入 | 无通配 | 同目录同包隐式互见（NLang的D7同源） |
| Python | `from m import x`→裸名；`import m`→限定 | from-import倾倒 | 后导入覆盖前者（静默，反例） |

结论：显式import授予裸（简单名）使用是Java/C#一致的主流语义；撞名一律使用点歧义错误点名双方，不静默挑选；遮蔽规则Java/C#互不一致且Java的「导入遮蔽本包」有历史包袱。NLang采「歧义即错」的严格侧。

**现代证据裁决（2026-11-04补搜后）**：V1与Rust use/TS命名导入/Swift/Kotlin完全一致；V4与Rust E0659/TS2300一致（使用点歧义错误点名双方，绝不静默挑选，限定/别名逃生）；V3获得比Java按需导入更强的现代锚点——ES模块规范**拒绝**通配裸倾倒、`import * as ns`恒限定（与NLang通配仅限定同构），Rust把glob压到低优先级且社区明诫慎用。反例仅Go/Zig的限定专用模型：它们自洽的前提是**全程限定**（库与工程包同一规则、无裸名惯例），恰是NLang现状两个世界割裂的反面教材——NLang的库包裸用已是事实惯例（根TU裸print/full遍布教程与测试），收敛方向只能是统一到裸用而非统一到限定。Rust「glob低优先级」在NLang中自然成立（通配不授裸名，优先级问题不出现）。现代证据对V1-V5零修正，设计定稿。

出处：JLS §7.5.1/§7.5.2（单类型/按需导入）、§6.5.5.1（简单名唯一可见否则编译错误）、§6.3.1（遮蔽），C# CS0104，Rust E0659/E0034，ES Module规范。检索记录：dev59.com/qEvSa4cB1Zd3GeqPbQ1（JLS 6.5.5.1简单名与遮蔽示例）、juejin.cn/post/6844904102619512845（单/按需导入机制与List歧义例）、dev59.com/jXvaa4cB1Zd3GeqPJ_en（按需导入遮蔽示例）、blog.gitcode.com/9b325ebfd69da0e2a223c57792f399bf.html（Rust E0659机制与修复）、dhexx.cn/hk/5390963.html（Rust glob低优先级与慎用诫）、qa.1r1g.com/sf/ask/3049797411/（ES模块拒绝通配裸倾倒）。

## 4 不合理点（各有独立理由，不以现状自证）

1. **可见性取决于调用方目录**（Q9）：`import io;`后裸`print`在根目录可用、子目录不可用。可见性规则不允许依赖调用文件的物理位置——这是`""`目录巧合的必然后果。
2. **显式导入对嵌套包是空操作**（Q6/Q7）：`import foo.bar;`之后仍必须写`foo.bar.x()`，import语句对调用形态没有任何作用，沦为注释。与库包行为（裸可用）自相矛盾，也与主流显式导入语义相悖。
3. **与两份设计文档同时矛盾**：D3（2026-08-28导入可见性设计：「跨模块bare合并移除」）与D10（MaybeLogPackageHint注释：「package function stays qualified-only even after the import」）都写明库函数限定专用，但根TU裸用库函数实际可用。代码与设计两头脱节，说明该行为从未被裁定过。
4. **诊断误导**（Q3）：拒绝时点名未导入的提供方（FindOwnedFunctionElsewhere自注"arbitrary pick among equals"）；建议"import it"在该场景无效（导入也不会令裸调用合法）；多提供方只报一个。
5. **文法与措辞缺陷**："is conflicted with a exist field definition"（语法错误＋函数被误标为field）；包提示不检查包是否已导入（M1的RED实测：`import lang;`在文件头仍建议"add 'import lang';"）。

合理且保留的部分：限定名绝对路径（无歧义构造）；通配不授裸名（D5防倾倒，撞名面为零）；同目录隐式互见（D7，Go同包风格）；重复包路径注册期拒绝。

## 5 统一模型裁决

**V1 裸名可见性（重构核心）**：函数在TU T中裸可见⟺无属主∨属主是T∨属主目录＝T目录（D7不变）∨**属主模块在T的显式导入列表（gate.exact）**。第四支取代目录巧合成为跨模块裸可见的唯一正门：显式`import a.b;`同时授予裸用与限定用，根TU与子目录TU、库包与工程包行为完全一致。适用面＝函数调用与函数值引用（`func<int()> f = x;`同走IsBareVisible，随门统一）；类型与全局字段不在内（V5）。

**V2 限定名**：不变。绝对模块路径，精确键查表，导入（显式或通配）准入。嵌套包下`foo.bar.x()`永远指顶层foo.bar，`my.foo.bar.x()`永远指绝对路径——通配不产生前缀别名，限定名构造上不可能歧义（Q2实证）。

**V3 通配**：不变。递归前缀（D5用户修订，与Java不递归不同，文档化差异）、仅限定调用、空并集报错、库包禁通配。显式导入给裸名、通配不给——分工：显式＝逐模块点名（撞名面可控），通配＝子树路径准入（零裸名倾倒）。

**V4 撞名消解**：
- 同目录池：声明期重复检查（既有，硬错误）。
- 跨提供方同名：按形状+距离合并重载（既有；fs.join/thread.join异形合并实证）。距离并列（含同形双提供方）＝**使用点歧义错误，点名全部提供方模块并给出限定逃生**（Java/C#严格侧，绝不静默挑选）。
- 本TU声明与导入同形：声明期硬冲突（既有）。不采Java「导入遮蔽本包」——遮蔽静默吞掉本地定义，响亮报错更合理。
- 限定名：不可能撞名（V2）。

**V5 类型与全局字段**：本批不动（既有v1范围界定：类型与字段全局可见，跨目录同名类冲突为已知限制）。理由：本批撞名关切是函数调用面；类型/字段可见性是独立门，列入后续工作。

**D-M1 Array(dims)内联字面量**（同机器缺陷修复）：`Array([2, 3])`的括号字面量是延迟init-list实参，构造拦截门因「无形参可推断」而整体拒绝。Array的dims形参已知为int[]（spec），拦截前给字面量喂int[]目标再走常规流程（TryInferArrayDimsDeferredArgs，镜像ResolveDeferredBraceArgs模式）。

## 6 诊断契约

| 场景 | 文案要点 |
|------|---------|
| 裸调用失败，名字仅存在于未导入模块 | 点名**全部**提供方模块；给出两个修法：显式导入（此后裸用与限定用均可）或直接限定调用。V1下freeFunc/class双文案合一 |
| 包提示且包已在导入列表 | 不提示导入（抑制），失败另有其原因 |
| 距离并列歧义 | "ambiguous call to function \"x\": 'a.b' and 'c.d' both match; qualify the call"——点名并列双方 |
| 声明冲突 | "conflicts with an existing definition"（修语法；按成员种类用词，不再把函数叫field） |

## 7 实现面

| 变更 | 位置 |
|------|------|
| ModuleRegistry::IsExactImportedBy(importer, owner)——gate.exact含owner路径（不含通配） | ModuleRegistry.h/.cpp |
| IsBareVisible加第四支 | ExprResolverMemberFields.cpp:177 |
| TryInferArrayDimsDeferredArgs＋ResolveFailedInvoke门改造（已按本设计起草于工作树） | ExprResolverNew.cpp、ExprResolverInvoke.cpp、ExprResolver.h |
| MaybeLogVisibilityHint多提供方＋导入感知 | ExprResolverMemberFields.cpp:239 |
| MaybeLogPackageHint已导入抑制＋双文案合一 | ExprResolverStdLib.cpp:429 |
| LogAmbiguousCall点名并列双方（MatchInvokeAgainst保留并列候选） | ExprResolverInvokeMatch.cpp、ExprResolverInvokeDiag.cpp |
| 冲突文案修正 | DuplicateFieldChecker.hpp:59,234 |

行为变化面：子目录TU裸用已导入库函数（Q8修复）；嵌套包显式导入后裸用（Q5修复）；上述诊断文案。无VM/字节码/ABI改动（解析期概念）。既有测试文案波及：3条manifest断言旧冲突文案（static_instance_collision、static_instance_collision_iface、thread_spawn_shadow），随新文案TDD更新；无测试断言"not visible here"文案。

## 8 测试矩阵

TDD全程。新增ctest夹具tests/e2e/proj_name_visibility/（main＋my/foo/bar.n＋foo/bar.n）：

1. 限定绝对性：`my.foo.bar.x()`＋`foo.bar.x()`并存各归其主（Q1/Q2正例固化）。
2. 显式导入裸用：`import foo.bar;`＋裸`x()`编译运行（Q5反转）。
3. 子目录TU＋`import io;`＋裸`print`（Q8反转）。
4. 通配仅限定：`import my.*;`＋裸`x()`拒绝且文案给出有效修法（显式导入或限定调用）。
5. 同形双导入歧义：`import foo.bar; import my.foo.bar;`＋裸`x()`→歧义错误点名双方；限定调用各自通过。
6. 异形合并：双导入异形`x()`按距离消解。
7. 本地声明与导入同形→声明期冲突（既有规则回归）。
8. 编译错误文案：包提示已导入抑制、并列歧义点名、冲突文法。
9. e2e：array_dynamic_subscript.n的`Array([2, 3])`（RED已写）。
10. 外部.ncu显式导入裸用（V1统一到external，回归既有外部模块测试）。

门禁：build ctest、全量e2e、IDE树ctest（编译器/导入变更必须）、Debug断言构建冒烟。

## 9 规格文档更新

语言规格（docs/site/manual/specification/）：

- 模块与导入页：模块路径＝目录点化；显式import授予裸用＋限定用；通配＝递归前缀、仅限定调用、空并集错误；重复包路径拒绝；限定名绝对路径、通配不别名前缀（用户场景my.foo.bar.x vs foo.bar.x的正面回答）。
- 名字解析页：裸名池＝同目录＋显式导入；优先序（本TU声明＞歧义即错）；撞名矩阵（异形合并/同形歧义/声明冲突/限定逃生）；类型与字段全局可见为已知限制。
- 已知限制核对：跨目录同名类冲突、共享命名空间跨目录成员（D系列遗留空洞）保持记载。

CHANGELOG随v0.12.7发布记录（Task 16统一提版）。

## 10 已知限制与后续工作

- 类型与全局字段的跨模块可见性门（V5，独立设计）。
- 通配是否授予裸名（Java按需给裸名＋歧义保护；D5明确不要，如改需用户裁定）。
- 共享命名空间跨目录成员可达形式（2028-08-28设计遗留，与类型门一并）。

## 附：M1起草状态

TryInferArrayDimsDeferredArgs已按本设计写入工作树（ExprResolver.h/ExprResolverNew.cpp/ExprResolverInvoke.cpp），e2e RED已写（array_dynamic_subscript.n，实测双重误导诊断）。设计获准后按TDD走完RED→GREEN再进入V1。
