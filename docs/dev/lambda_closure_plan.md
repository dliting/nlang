# NLang Lambda函数/闭包实施计划（v0.13.10）

> **For agentic workers:** 本计划按任务逐个执行（superpowers:executing-plans内联执行）。步骤用checkbox（`- [ ]`）跟踪。

**目标：** 落地docs/dev/lambda_closure_design.md定义的lambda表达式/闭包，并清偿泛型函数值解禁两笔挂账。

**架构：** 覆盖文法（内层按表达式列表解析，`)`处lookahead `->`分流）→ 期望类型通道类型化 → 统一display class降阶（捕获变量存储改道display字段，lambda体降阶为display实例方法，函数值=OP_MakeBoundFunc）→ 函数值记录3槽扩4槽配对实例化描述符。

**技术栈：** Flex/Bison（nlang.l/nlang.y）、C++17编译器（AST+resolver+VmBackend）、字节码VM、NCU v2.23。

**Spec：** `docs/dev/lambda_closure_design.md`（三轮评审干净退出，提交8d2635d7）。计划与spec冲突时以spec为准。

## 全局约束（spec逐条摘录）

- 按引用捕获：同一变量所有捕获点与外层读写共享同一存储（spec §3.3）。
- 循环头变量每次迭代新鲜：for/foreach头部声明变量每迭代是新变量（spec §3.3、D2）。
- 统一display降阶、零捕获同路径、**零新增操作码**（spec §4.1、D3）。
- 函数值记录3槽`{target,this,form}`扩4槽`{target,this,form,descIdx}`；`OP_MakeFunc`追加u16 descIdx操作数，哨兵`kInstDescNone=0xFFFF`；全部`OP_MakeBoundFunc`路径descIdx恒填0xFFFF（spec §4.2、D4）。
- NCU v2.23，变更：MakeFunc操作数形状＋函数表lambda标志位（spec §4.3）。实现中若引入新RTK字段种类值（display基句柄字段），随同版本提升记录进手册§26——不额外开版本号。
- toString渲染`"lambda"`，判据＝函数表lambda标志位，不依赖合成名前缀（spec §3.4、D5）。
- 闭包身份＝(lambda方法，display实例)；同一次作用域激活内两求值相等、跨激活不等（spec §3.4、D12）。
- 显示计划停在详细设计层（用户裁定2026-11-15）：任务给锚点/接口/测试，不内嵌实现代码全文。
- 命名遵守repo约定（类型PascalCase、方法/变量camelCase）；注释英文；诊断文案逐字进测试断言。

## 评审关注点（spec未直言、最可能咬人的五类输入）

1. **`(x)`单参lambda与括号表达式同形**——`func<int,int> f = (x) -> x;`必须成lambda，`int y = (x);`必须成括号表达式；分流只看`)`后的`->`。测试钉在Task 2（解析分流）。
2. **`(a, b)`两元素列表在非lambda位置**——`int p = (a, b);`今天就该是语法错误，覆盖文法不得让它悄悄变合法。测试钉在Task 2。
3. **display分配遇早退作用域**——被捕获作用域内有`return`/`break`提前离开时，display分配仍须在入口完成（一次性分配，无路径分叉），Task 6验收。
4. **递归lambda自引用**——`func<int,int> f = (x) -> …f(…)…;`被捕获的`f`在lambda创建前已提升（spec §3.3），Task 6/11双钉。
5. **泛型display的实例化行**——`OP_New`的descIdx只认静态行或0xFFFF（VmExecutorOpsObjects.cpp:149-159），泛型display必须走TEMPLATE_HEAD＋ARG_CONTEXT组合行，不得按字面复用外层实例化行（spec §3.6）。测试钉在Task 10。

---

### Task 1: 词法记号`->`

**Files:**
- Modify: `src/compiler/grammar/nlang.l:347-350`（OT_LE/OT_GE/OT_NE/OT_EQ算符块）
- Modify: `src/compiler/grammar/nlang.y:458`（%token声明区）
- Test: `tests/test_compiler/test_scanner.cpp`（scanner测试族实际位置——评审I13）

**Interfaces:**
- Produces: 记号`OT_Arrow`（拼写`->`），供Task 2文法引用。
- 消费约束：`==`同首字符由flex最长匹配第二字符分流；`>=`首字符不同（spec §3.1词法论据）。

**Steps:**
- [ ] **1.1 写失败测试**：scanner单测（tests/test_compiler/test_scanner.cpp，即Files所指测试族——评审N2）断言`"->"`产出OT_Arrow、`"=="`仍产出OT_EQ、`">="`仍产出OT_GE、`"->="`产出OT_Arrow+OT_Assign（最长匹配边界）。
- [ ] **1.2 跑测试确认RED**（OT_Arrow不存在，编译失败即预期形态）。
- [ ] **1.3 实现**：nlang.l算符块加规则；nlang.y声明`%token <ival> OT_Arrow`（槽类型随既有OT_*同族）。
- [ ] **1.4 跑测试确认GREEN**；跑既有parser测试族无回归。
- [ ] **1.5 提交**：`feat(lexer): the -> arrow token`。

### Task 2: 覆盖文法与SnLambdaExpr节点

**Files:**
- Modify: `src/compiler/grammar/nlang.y:2717`（Expression备选）、`:2861`（ParenthesesExpr）
- Create: `src/compiler/SnExpressionsLambda.cpp`（新节点实现，SnExpressions.cpp同族）
- Modify: `include/nlang/compiler/SnExpressions.h`（节点声明）
- Modify: `include/nlang/compiler/SyntaxNodeConsts.h:16-66`（COMPILE_ONLY_NODE_DECL加LambdaExpr）
- Test: 既有parser单测族＋新增lambda解析用例

**Interfaces:**
- Consumes: Task 1的OT_Arrow。
- Produces: `SnLambdaExpr`（NK_LambdaExpr）——形参表`std::vector<LambdaParam>`（`LambdaParam{std::string name; SnFieldExpr* pType; /*pType=nullptr=省类型*/}`）、体（`SnExpression*`表达式体或`SnParagraph*`块体，二择一非空）、`InferredTarget(SnField*)`期望类型槽（SnInitListExpr:1156同通道形态）。后续Task 4/6/7按此消费。

**文法形状（spec §3.1/D9，统一前缀形态）：**
- `ParenPrefix: '(' InnerList ')' ;` `ParenthesesExpr: ParenPrefix | LambdaExpr ;` `LambdaExpr: ParenPrefix OT_Arrow LambdaBody ;`
- `InnerList`允许空、元素为`Expression | TypeRef TT_Identifier`；`LambdaBody: Expression | Block`。
- **为何必须统一前缀（评审M1修正）**：若保留独立臂`ParenthesesExpr: '(' Expression ')'`，则`InnerElem: Expression`使LR状态`'( Expression .`在lookahead `)`处同时持有shift `)`（续括号臂）与reduce `InnerElem → Expression`＝shift/reduce冲突，bison默认shift导致`(x) -> x`语法错误。统一前缀让`'(' InnerList ')'`整体先规约成ParenPrefix（括号表达式与lambda参数表共用此前缀），随后lookahead `OT_Arrow`→shift续LambdaExpr、其余→reduce入ParenthesesExpr——`->`不入任何表达式FOLLOW集（spec D9论据）故无冲突。AST构造：ParenPrefix reduce时先造「括号组」中间形态（保留InnerList原始元素），`->`确认后重组为SnLambdaExpr参数表并做形态后检（Task 3）；无`->`时按括号表达式语义后检四分支（恰一个Expression元素＝其值；**空列表＝拒绝**——`()`今天是语法错误（ParenthesesExpr要求Expression），统一前缀后`()`能完成ParenPrefix规约，必须由语义后检拒绝空括号组，否则悄悄变合法（评审N3b）；**单TypeRef元素＝拒绝**——`(int x)`无`->`在统一前缀下文法可达（InnerList的TypeRef臂），后检按非lambda位置拒绝（评审N3c）；多元素在非lambda位置＝既有「非法表达式」错误——评审关注点2由此后检钉住）。
- 验收＝bison零新增冲突（构建日志比对）＋既有语法全绿。

**Steps:**
- [ ] **2.1 写失败测试**：解析层用例——`(x) -> x*2`、`(x, y) -> x+y`、`(int x) -> x`、`(int& x) -> x`（&形参形态，spec §3.3）、`(Point p) -> p.x`、`() -> 1`、`(x+y) -> x`（Expression臂参数，后检路径）、块体`(x) -> { return x; }`各成SnLambdaExpr且参数表/体形状正确；反向用例——`int y = (x);`仍是括号表达式（非lambda）、`int p = (a, b);`仍被拒绝（统一前缀下`(a,b)`文法层可完成规约，拒绝时点移至语义后检，错误类别从syntax error变为语义诊断——评审N3a；若既有诊断用例逐字断言syntax error文本须同步迁移断言，2.4全parser族绿时核对）、`int e = ();`被拒（空括号组，评审N3b）、`int t = (int x);`被拒（单TypeRef元素无`->`，评审N3c）。RED形态＝节点不存在，编译失败。
- [ ] **2.2 确认RED**。
- [ ] **2.3 实现**：节点类（参照SnBinaryExpr:823的子槽+ReplaceChildNode范式；参数表用UniquePtrList挂子）；文法产生式（`(x,y)`列表按表达式列表规约，`(int x)`类型关键字分流、`(Point p)`ident后跟ident只有TypeRef TT_Identifier路径可活——spec D9）；块体复用Paragraph产生式。**注意**：`(`后跟类型关键字（KT_Int等）进入TypeRef路径时禁止悬垂——若`)`后无`->`必须报既有「表达式位置非法」诊断而非崩溃。旧文法下此形态不可达（`(int x)`在表达式文法中本就不合法）、无既有行为需保持；统一前缀下它恰是文法可达、由语义后检拒绝——这正是覆盖文法的设计意图（评审N3c）。
- [ ] **2.4 确认GREEN**＋bison冲突计数零新增＋全parser族绿。
- [ ] **2.5 提交**：`feat(parser): lambda cover grammar and the SnLambdaExpr node`。

### Task 3: 参数形态语义后检与诊断族

**Files:**
- Modify: `src/compiler/builder/ExprResolver.cpp`（SnLambdaExpr的Access臂入口）
- Create: `src/compiler/builder/ExprResolverLambda.cpp`（lambda解析拆出文件，ExprResolverInitList.cpp同族先例）
- Test: 诊断文本逐字断言用例（spec §3.5）

**Interfaces:**
- Consumes: SnLambdaExpr（Task 2）。
- Produces: 后检谓词`InvalidLambdaParameterList(SnLambdaExpr&)`（覆盖列表元素非裸标识符/类型形参形态→`invalid lambda parameter list`）；默认值形参诊断`a lambda parameter cannot have a default value`。诊断进CompileLogger，Task 4复用同一入口。

**Steps:**
- [ ] **3.1 写失败测试**：`(1) -> 1`、`(x.y) -> 1`、`(f()) -> 1`各报`invalid lambda parameter list`；`(x = 3) -> x`报`a lambda parameter cannot have a default value`（文法无默认值位，形态经覆盖列表以赋值表达式到达后检——若实测不可达则按spec §3.5在`(InnerElem=Expression)`形态处点名）；spec §3.5清单的「lambda带泛型参数表（`<T>`）」确认文法无此位（覆盖文法下`<`按算符进InnerElem=Expression路径，到达后检报invalid lambda parameter list）。
- [ ] **3.2 确认RED**（当前无诊断，解析通过或报无关错误）。
- [ ] **3.3 实现**：`->`确认后对InnerList逐元素形态判定——裸标识符（省类型）或`TypeRef TT_Identifier`（显式）之外全拒。
- [ ] **3.4 确认GREEN**。
- [ ] **3.5 提交**：`feat(resolver): lambda parameter shape post-check`。

### Task 4: 期望类型通道与类型化

**Files:**
- Modify: `src/compiler/builder/ExprResolverLambda.cpp`（Task 3创建）
- Modify: 期望类型压入点——局部声明初始化器（StatementResolverDecl*）、赋值（StatementResolverAssign.cpp）、return（返回类型通道）、调用实参（ExprResolverInvokeMatch.cpp）、数组元素/花括号条目（ExprResolverInitList.cpp:374-390既有InferredTarget下推点）
- Test: 类型化用例族

**Interfaces:**
- Consumes: SnLambdaExpr::InferredTarget（Task 2声明、本任务接线）。
- Produces: 解析写回——形参类型槽填充（省类型按期望逐位置填充）、节点`NF_Resolved`＋解析出的`func<R,P…>`类型token。Task 6消费`NF_Resolved`的SnLambdaExpr做display合成。

**类型化规则（spec §3.2）：**
- 期望类型必须是func类型（`func<R,P…>`，nlang.y:2525 MakeFuncType产物）；无期望类型＝编译期错误，诊断点名lambda。
- 显式形参类型与期望**逐位置完全一致**（无变异性）。
- 期望`func<void,…>`配非void表达式体＝编译错误（D6，不做静默丢弃）；块体无return即void。
- 字段初始化器位置拒绝（常量表达式规则，spec §3.2）——诊断引用§4.8.3/§4.7.2规则名。
- 表达式体lambda的类型＝表达式类型。

**Steps:**
- [ ] **4.1 写失败测试**：`func<int,int> f = (x) -> x*2;`类型解析成功；`(x) -> x`裸表达式语句报「lambda需要期望类型」族诊断；`func<int,int> f = (float x) -> x;`报显式不一致；`func<void> g = () -> 1;`报void丢弃；`func<int,int> f = () -> { f(); };`块体void成形；字段`func<int,int> m = (x) -> x;`报字段初始化器拒绝；`func<int&,int&> f = (int& x) -> x;`的&形参&返回类型化成形（spec §3.3「lambda的&形参与&返回」——引用形参/返回在类方法上既有实现照搬§4.7.9、§5.9）。
- [ ] **4.2 确认RED**。
- [ ] **4.3 实现**：五个压入点把期望类型写进InferredTarget（InferredTarget通道既有形态：parent-set、bare form、解析时读）；lambda解析臂读期望、校验、填充形参类型、写回类型token。
- [ ] **4.4 确认GREEN**＋既有初始化器/调用族无回归。
- [ ] **4.5 提交**：`feat(resolver): lambda typing through the expected-type channel`。

### Task 5: 函数值记录4槽＋MakeFunc描述符配对＋泛型函数值解禁（NCU v2.23）

**Files:**
- Modify: `src/vm/VmExecutorOpsCalls.cpp:53`（OpMakeFunc：isGeneric拒绝→读u16 descIdx操作数）、`:95`（OpMakeBoundFunc：AllocFuncRecord填0xFFFF）、`:245`（AllocFuncRecord 3→4槽）、`:334`（CallDelegateFree：isGeneric拒绝→从句柄descIdx填充被叫帧描述符单元）、`:385-393`（ExecuteDelegateCall句柄布局注释385-392——评审L7修正：381是CallDelegateBound内调用行）
- Modify: `src/vm/VmExecutorContainers.cpp:214`（FuncRecordValuesEqual比cells 0..3）
- Modify: `src/vm/VmExecutor.h:2017`（记录形状注释同步，spec §4.1清单）
- Modify: `src/vm/BytecodeOps.h:134`（MakeFunc操作数注释）、`src/vm/DisassemblerCalls.cpp:95`（反汇编操作数）、`src/vm/NcuLinkerRemap.cpp:50`（链接重映射）
- Modify: `include/nlang/vm/CompiledModule.h`（NCU_FORMAT_MINOR 22→23＋版本注释行）、`include/nlang/vm/CompiledModuleRecords.h`（CompiledFunction加`bool isLambda = false;`；序列化/加载两侧）
- Modify: 发射侧`src/vm/backend/`（OP_MakeFunc发射点加u16操作数；函数值引用泛型目标时由期望类型推断实例化、发射组合descIdx——推断复用泛型调用点既有推断通道ExprResolverGenericUse.cpp族）
- Modify: 0.13.1a具名拒绝诊断退役——**实测定位清单**（评审M2修正：spec §4.2原文引的resolver侧拼写失实）：(a)泛型类方法臂resolver文本为`a function of a generic class cannot be referenced as a value`（RejectGenericRecordTarget，ExprResolverFuncRef.cpp:82-85）；(b)泛型**游离**函数值引用的resolver拒绝位点在ExprResolverFuncRef.cpp的RejectFuncRefTarget四处gate中未见isGeneric检查——实现期第一步先实测钉位（写`T max<T>`赋`func`期望的探针看哪个gate/还是仅VM后stop拦），钉位后再定退役面；替换为spec §3.5新诊断族；VM防御后stop文本（VmExecutorOpsCalls.cpp:64-70/338-341）随descIdx解禁改为「描述符缺失才拒」的收窄形态
- Modify: `src/vm/NcuLinkerRemap.cpp:130-162`（PatchDescriptorOperands新增`case OP_MakeFunc: descOff = 3;`——评审M5：MakeFunc今日只在:50的函数索引通道case里，加descIdx词位后链接重映射必须认它，否则跨NCU链接的泛型函数值静默拿错描述符行）
- Test: tests/test_vm单测（记录形状、比较器4元组、MakeFunc操作数roundtrip、**跨模块链接的泛型函数值重映射**）＋e2e泛型函数值族

**Interfaces:**
- Consumes: 无前置任务依赖（与Task 2-4并行可行）。
- Produces: 4槽记录 ABI；`OP_MakeFunc funcIdx descIdx`双u16操作数形状；泛型游离函数/泛型静态方法的函数值解禁。Task 7的MakeBoundFunc记录消费同一AllocFuncRecord（descIdx恒0xFFFF）。

**解禁验收（spec §五）：**
- `T max<T>(T a, T b)`赋给`func<int,int,int>`后调用正确；
- 泛型静态方法引用同通道；
- 两个不同实例化的句柄`!=`、同源两引用`==`（descIdx参与相等——spec §3.4）。

**Steps:**
- [ ] **5.1 写失败测试**：单测——AllocFuncRecord四槽形状断言；FuncRecordValuesEqual含descIdx维度（同target/this/form不同descIdx→不等）；MakeFunc双操作数roundtrip（发射→反汇编字符串断言）；**跨NCU链接用例**：模块A引用模块B的泛型函数值，链接后断言descIdx重映射到合并表正确行（评审M5——单模块roundtrip抓不住链接词位错误）。e2e——泛型函数值族三形态。RED＝编译失败或断言不符。
- [ ] **5.2 确认RED**。
- [ ] **5.3 实现**：按Files清单逐点；NCU v2.23注释行记两处变更（MakeFunc操作数、函数表lambda标志位——标志位本任务只加序列化位与加载，渲染消费在Task 7）；`loader_rejects_v2_22`类门（参照既有loader_rejects_v2_2x单测族）。
- [ ] **5.4 确认GREEN**＋全量Debug单测（NCU格式变更波及面大，跑全量）＋**重链后重建stdlib预置包**（既有陈旧工具守卫经验：格式变更后旧.npkg必假败）。
- [ ] **5.5 提交**：`feat(vm)!: 4-slot function records with paired instantiation descriptors (NCU v2.23)`。

### Task 6: display合成与捕获提升

**Files:**
- Create: `src/compiler/builder/LambdaCaptureAnalysis.cpp/.h`（捕获分析＋display合成pass，resolver末段/codegen前）
- Modify: `src/compiler/builder/StatementResolver*`（被捕获变量标记——新存储类别，见下）
- Modify: `src/vm/backend/`（被捕获变量访问改道发射：标识符臂读display字段；声明语句臂跳过普通槽；**&绑定发射臂**EmitExprRef.cpp:35起EmitMakeRefOfIdentifier的display字段目标形态——评审L11）
- Modify: `include/nlang/compiler/SnStatements.h`（SnLocalVar/declarator的被捕获标记——**位预算警示**：NODE_FLAG_BITS=29已满（NodeConsts.h:20-29），新标志须先扩位预算29→30并核对打包上限）
- Test: 单测（display字段集形状、降阶产物）＋e2e捕获族

**Interfaces:**
- Consumes: `NF_Resolved`的SnLambdaExpr（Task 4）；NF_Promoted提升机制先例（RefBindJudgment.cpp:275设置点、EmitStmtDecl.cpp:57等消费点——**不复用NF_Promoted本身**，被捕获是新存储类别「display字段」，与&提升正交）。
- Produces: 每个被捕获作用域一份display合成类（合成类按spec §4.1挂在**被捕获变量的声明作用域**＝SnParagraph（SnStatements.h:93，函数体段落/块段落/循环体段落）；零捕获lambda的display挂其所在作用域；多作用域捕获时内层display持外层display引用字段）；SnLambdaExpr写回display类引用＋合成方法引用。Task 7消费。

**降阶不变量（spec §4.1，评审关注点3）：**
- display分配在作用域入口一次完成（无路径分叉）；早退（return/break/continue）不影响分配事实。
- 被捕获变量的**所有**访问（外层读写＋lambda体内）寻址同一display字段。
- `this`捕获＝display加接收者字段；`&`捕获＝基句柄＋偏移两个相邻display字段位（D11）；static局部不入display（直接寻址静态槽，spec §3.3）。
- **静态上下文检查不得被改道绕过**（评审M4）：lambda降阶为display实例方法后，体内`this`访问经display接收者字段——静态上下文（静态方法/游离函数）内引用`this`的既有编译错误必须在改道**之前**判定（spec §3.5错误清单「静态上下文捕获this」），测试钉住改道不吞该错误。
- 被捕获变量自身成为`&`绑定目标或返回其引用时，`EmitMakeRefOfIdentifier`（src/vm/backend/EmitExprRef.cpp:35起，现有NF_Ref/NF_Promoted/plain三臂）需新增display字段目标形态（评审L11）——字段寻址产生引用对。
- 循环体作用域每次迭代重新激活（Task 9细化值链接续）。
- 合成类名编译器内部命名（不进用户可见诊断）。
- display合成类字段集单测断言（spec §五单元项：「display字段集、MakeBoundFunc位点、byref字段标记臂」）。

**Steps:**
- [ ] **6.1 写失败测试**：单测——`func<int,int> f = (x) -> x + n;`（n为外层int）合成display含字段n；两个lambda共享捕获变量合成同一display（兄弟块共享外层display）；`&`捕获字段集含基句柄＋偏移双位；**早退作用域**（被捕获作用域内`if (…) return;`分支后lambda仍可用、display分配不分叉——评审关注点3钉满）；**递归lambda**`func<int,int> f = (x) -> x <= 1 ? 1 : x * f(x - 1);`合成形状（f被捕获先于lambda创建——评审关注点4的Task 6半边）；**静态上下文`this`捕获→编译错误**（静态方法内lambda引用this成员）。e2e先行（TDD外层钉行为）——计数器跨调用保持、两lambda共享存储（f1改f2见）。RED＝e2e编译失败（lambda尚无降阶）。
- [ ] **6.2 确认RED**。
- [ ] **6.3 实现**：捕获分析walk（SnLambdaExpr体内标识符/this解析目标回溯声明作用域）；display合成（VmBackend.cpp:69 $staticInit协议臂先例——合成类进类表、合成方法进函数表）；被捕获变量存储改道（声明→display字段初始化、访问→字段寻址）。位预算扩容若需要，独立微提交先行。
- [ ] **6.4 确认GREEN**（单测＋捕获族e2e）＋**加跑T1停靠层**（被捕获变量堆化＋存储改道属内存风险族，评审M6）。
- [ ] **6.5 提交**：`feat(compiler): display-class synthesis and capture promotion`。

### Task 7: 闭包发射、相等性与toString

**Files:**
- Modify: `src/vm/backend/`（作用域入口display分配OP_New＋隐藏display槽；SnLambdaExpr发射＝OP_MakeBoundFunc（display实例为接收者））
- Modify: `src/vm/VmExecutorFormat.cpp:41-58`（FormatFuncHandle渲染本体——评审L8/I15修正：~75是OpFunc_to_str内的调用行:76，渲染分流改本体；isGeneric标志位消费同款形态）
- Test: e2e（toString、闭包相等性）＋单测

**Interfaces:**
- Consumes: Task 5的4槽AllocFuncRecord（descIdx恒0xFFFF）与isLambda序列化位；Task 6的display合成产物。
- Produces: 完整闭包值。相等性免费来自FuncRecordValuesEqual四元组（receiver=display实例，spec §3.4）。

**验收（spec §五）：**
- `toString`=="lambda"；
- 同一次作用域激活内同一lambda两求值`==`、工厂函数两次调用（跨激活）`!=`（D12：零捕获同规，「!=」断言须用跨激活形态）；
- `thread.spawn(() -> …)`零参lambda；
- lambda入`List<func<…>>`与`contains`/`indexOf`。

**Steps:**
- [ ] **7.1 写失败测试**：上述四形态e2e。RED＝行为缺失。
- [ ] **7.2 确认RED**。
- [ ] **7.3 实现**：入口分配＋发射臂＋渲染分流（FormatFuncHandle内isGeneric标志位消费同款形态，VmExecutorFormat.cpp:41-58）。
- [ ] **7.4 确认GREEN**。
- [ ] **7.5 提交**：`feat(vm): closure emission, equality and lambda toString`。

### Task 8: GC基句柄标记臂

**Files:**
- Modify: `include/nlang/vm/CompiledModuleRecords.h`（新RTK常量：display基句柄字段种类，命名随RTK_Byref/RTK_ByrefCell族——如`RTK_ByrefBase`；注释记录「仅display合成字段声明此种类」与RTK_Byref=22「绝不出现在字段上」的边界相对）
- Modify: `src/vm/VmExecutorGCMarks.cpp:45`（MarkClassFields新臂：基句柄槽走TestRootValue保守双域路由——RTK_TypeParam同款，VmExecutorGCMarks.cpp:60-64先例）
- Test: tests/test_vm GC单测（byref逃逸display存活）

**Interfaces:**
- Consumes: Task 6的双字段位布局（基句柄字段声明种类=新RTK值；偏移字段声明RTK_Int32，天然不标记）。
- Produces: GC安全性；e2e`&`捕获逃逸后调用（spawn回收写累加）的底层保障。

**为何必须新臂（设计核实）**：DeclaredFieldAcceptsRuntime（VmExecutorGCMarks.cpp:31-40）的RTK_Class臂只收{Class,Boxed,Array,ArrayView}——而`&`目标的基句柄按构造可为RTK_Struct（唯一来源＝`&`结构局部——OP_RefOfLocal直接绑堆记录；结构值容器元素的基句柄是容器记录（List/Array等，RTK_Class）而非结构记录本身，不在RTK_Struct来源内——评审N4收窄。RefBindJudgment.cpp:275的提升只覆盖标量/字符串/枚举，结构走堆记录不提升），泛用RTK_Class声明字段会欠标记致提前回收（评审复核修正：`&`字符串目标按构造是RTK_Boxed提升盒、RTK_Class臂可标记——该半句不构成论据；TestRootValue保守双域路由一次性覆盖全部基句柄kind，仍是正确方案）。

**Steps:**
- [ ] **8.1 写失败测试**：GC单测——display持`&`结构引用、lambda逃逸后churn，断言被引结构存活（RED＝欠标记提前回收；**确定性触发**用无旗标形态＋churner拉宽回收窗——既有gc-stress经验：--gc-stress微回收反而是调度屏障会压窄竞态窗，评审L10）。
- [ ] **8.2 确认RED**。
- [ ] **8.3 实现**：常量＋标记臂（偏移字段无标记路径，确认不需要改）。
- [ ] **8.4 确认GREEN**＋既有GC族全绿＋**加跑T1停靠层**（GC标记臂属内存风险族，评审M6）。
- [ ] **8.5 提交**：`feat(gc): mark display byref-capture base handles`。

### Task 9: 循环头变量每迭代新鲜

**Files:**
- Modify: `src/compiler/builder/LambdaCaptureAnalysis.*`（Task 6创建；for/foreach头变量归属每迭代激活的循环体display）
- Modify: `src/vm/backend/EmitStmtForeach.cpp`（foreach头display激活衔接）与for发射臂
- Test: e2e（for/foreach各一，断言非尾值）

**Interfaces:**
- Consumes: Task 6/7的display机制。
- Produces: 循环捕获语义（D2）。实现形态＝Go 1.22值链：迭代末把当前display的被捕获头变量值接续进新display（spec §3.3）；条件段/递增段寻址当前迭代实例。

**Steps:**
- [ ] **9.1 写失败测试**：e2e——for与foreach各一：循环内建lambda捕获头变量入列表，循环后逐个调用断言各持当次值（非尾值）。RED＝共享尾值。
- [ ] **9.2 确认RED**。
- [ ] **9.3 实现**：循环体display每迭代重激活＋值链接续发射。
- [ ] **9.4 确认GREEN**。
- [ ] **9.5 提交**：`feat(vm): per-iteration loop-head capture freshness`。

### Task 10: 泛型上下文display

**Files:**
- Modify: `src/compiler/builder/LambdaCaptureAnalysis.*`（display按泛型类模板合成——携带外层函数全部类型参数，字段类型可为T）
- Modify: `src/vm/backend/`（display的OP_New走TEMPLATE_HEAD＋ARG_CONTEXT组合行——VmExecutorOpsDescriptor.cpp:25-80机制；**不得**按字面复用外层实例化行，OP_New descIdx只认静态行或0xFFFF，VmExecutorOpsObjects.cpp:149-159）
- Test: e2e泛型体内lambda捕获T变量

**Interfaces:**
- Consumes: Task 6/7；泛型实例化既有机制（ComposeInstDescriptor、绑定头描述符通道VmExecutorFrames.cpp:120-127）。
- Produces: `func<void> make<T>(T x) { return () -> print(x); }`全链（spec §3.6）。闭包值描述符经接收者头填充——descIdx槽恒0xFFFF不冲突（Task 5已定）。

**边界（spec §3.6）**：泛型体内static局部禁令照旧（含lambda降阶体）；引用外层static局部走静态槽直达。

**Steps:**
- [ ] **10.1 写失败测试**：e2e——`make<T>`实例化`int`与`string`两态，断言T操作各拿正确描述符。RED＝不支持。
- [ ] **10.2 确认RED**。
- [ ] **10.3 实现**：模板合成＋组合行发射。
- [ ] **10.4 确认GREEN**＋既有泛型族无回归。
- [ ] **10.5 提交**：`feat(generics): displays in generic context via composed instantiation rows`。

### Task 11: e2e lambda族收口

**Files:**
- Create: `tests/e2e/`下lambda族`.n`源＋`tests/e2e/manifest.txt`登记（行格式`name expected_rc`）
- Modify: `tools/test_families.txt`（新e2e族入影响映射）

**Interfaces:**
- Consumes: Task 2-10全部。
- Produces: spec §五lambda族12条全覆盖——计数器跨调用保持；两lambda共享存储；循环新鲜（for/foreach，Task 9已建，此处收口进族）；递归lambda；`thread.spawn(() -> …)`；`List<func>`容器；toString；同激活`==`/跨激活`!=`；`&`捕获逃逸；返回被捕获变量引用（`func<int&> f = () -> r;`放行，spec §3.3）；`func<int&,int&> f = (int& x) -> x;`的&形参&返回全链（评审M3——Task 4类型化之外补运行时行为）；泛型体内lambda（Task 10已建）；另钉spec §3.3尾条——`const`局部捕获后lambda内赋值仍是编译错误（捕获不改变声明语义，NF_Const位沿用）；静态上下文lambda引用`this`→编译错误（评审M4运行时面：Task 6单测钉编译期，此处e2e钉端到端）。

**Steps:**
- [ ] **11.1 写失败测试**：族清单逐条落`.n`＋manifest行（部分已在Task 6-10先行，此处补齐收口并统一断言强度）。
- [ ] **11.2 确认RED→GREEN**（先行任务已绿的条目确认无回归）。
- [ ] **11.3 提交**：`test(e2e): the lambda closure family`。

### Task 12: 手册同步与版本收口

**Files:**
- Modify: 手册§7.4整节（lambda语法/捕获/相等/toString/错误清单）、§2文法概览、§5.9函数值表达式、§6.1 static局部与lambda、§26 NCU格式（v2.23、MakeFunc操作数、函数表标志位——泛型旗标行zh:91/en:147同位、若引新RTK字段种类值一并记录）、`stdlib/http.n:1357-1360`注释（「The language has no anonymous function literals」失实，且内文「lambda is the separate 0.13.3 subproject」版本号过时一并更正——评审I14）、§18 Promise页`thread.spawn(() -> …)`表述——中英成对
- Modify: `VERSION`（0.13.10）、`CHANGELOG.md`（中英）、`docs/user_manual/SYNC_BASELINE`流程（`tools/manual_sync_check.py`先行，提交后`--update`）
- Test: docs五腿门禁

**Steps:**
- [ ] **12.1 手册七点逐条编辑**（先跑`tools/manual_sync_check.py`核基线漂移）。
- [ ] **12.2 VERSION/CHANGELOG**提升。
- [ ] **12.3 提交**：`docs(manual): lambda closures in the specification`＋`chore(release): 0.13.10`。
- [ ] **12.4 基线刷新**：手册提交后`--update`，核对no drift。

---

## 任务依赖图

```
Task 1 → Task 2 → Task 3 → Task 4 → Task 6 → Task 7 → Task 8 → Task 9 → Task 10 → Task 11 → Task 12
                                              (Task 5 与 2-4 并行；Task 7 消费 5+6)
```

## 门禁节奏（依用户裁定：本分支功能测试止于T0/T1层，完整套件随dev改造采纳）

- 每任务收尾：T0影响映射Debug层（`tools/gate_tier.sh T0`）。
- Task 5（NCU格式变更）、Task 6与Task 8（内存风险族）与Task 12：加跑T1停靠层（与任务步骤6.4/8.4一致——评审N1）。
- 慢测试与ASan/IDE树全量＝release级T2，仅版本收口一次。
