# NLang Lambda函数/闭包设计（v0.13.10）

- 版本：v0.13.10（roadmap 2026-11-20合并视图）
- 状态：概要设计（评审循环第三轮干净退出：三轮发现全部落实，末轮零新Medium+）
- 前置：`func<R, P…>`函数值与委托（§7.4，Phase 13）；运行时泛型类型参数信息（v0.13.1）；`&`记法（v0.13.9）

## 一、目标与范围

1. **lambda表达式**——带捕获的匿名函数，成`func<R, P…>`值：`func<int,int> f = (x) -> x * 2;`。
2. **清偿泛型完善两笔挂账**（docs/dev/generic_completion_design.md）：泛型游离函数/泛型静态方法的函数值解禁（描述符配对存储）；函数值表示随配对演进。
3. **铺垫0.13.12 web框架回调**（roadmap：闭包落地后替换绑定方法+字段的状态持久写法）。

非语言面目标（不改）：函数类型文法、函数值null/装箱规则（§7.4既有语义全部沿用）。

## 二、主流方案调研（裁决依据）

| 语言 | 捕获语义 | 实现 | 循环变量 |
| --- | --- | --- | --- |
| C# | 按引用（变量而非值） | display class提升：捕获变量提升为编译器生成类字段，lambda体成其实例方法，委托持有(target=display实例, method) | C#5起foreach每迭代新鲜；C式for的头部变量长期共享（著名陷阱） |
| Go | 按引用 | 闭包对象 | 1.22起循环头声明变量每次迭代新鲜（隐式拷贝），修复十年陷阱 |
| Kotlin | var按引用（Ref包装器），val按值 | 包装器类(ObjectRef/IntRef) | — |
| Java | 按值且限effectively final（禁改捕获变量） | LambdaMetafactory | — |

共识：**可变捕获＝引用语义＋堆存储**（C#/Go/Kotlin三家一致，Java的final限制是历史包袱而非推荐面）；**循环头声明的变量按迭代新鲜**是现代共识（Go 1.22、C# 5+foreach）。出处：Eric Lippert对display class机制的说明与C#5 foreach变更、go.dev/blog/loopvar-preview、Kotlin in Action对Ref包装器的记载。

## 三、语言面

### 3.1 语法

```
LambdaExpr: '(' InnerList ')' '->' LambdaBody
InnerList:  InnerElem { ',' InnerElem } | /* 空 */
InnerElem:  Expression | TypeRef TT_Identifier
LambdaBody: Expression | Block
```

- **覆盖文法（cover grammar）消解LALR冲突**（D9）：内层统一按表达式列表解析——`(x)`按括号表达式、`(x, y)`按列表规约，`->`不处于任何既有表达式的FOLLOW集，`)`处由lookahead`->`分流（shift向lambda、否则规约括号表达式），LALR(1)无冲突。参数形态在`->`确认后语义后检：列表元素须为裸标识符（省类型形参）或`TypeRef 标识符`（显式形参），其余报「invalid lambda parameter list」；`TypeRef 标识符`对偶形态（如`int x`）在表达式文法中本就不合法，`(`后见类型关键字自然分流，不产生新冲突；自定义类型名形态（如`(Point p)`）中ident后跟ident在表达式文法中无去处、只有`TypeRef TT_Identifier`路径可活，同样分流。
- 记号`->`为新增双字符算符；`nlang.l`现无该记号。词法无歧义的完整论据：`->`与同首字符的`==`在flex最长匹配下由第二字符分流；与`>=`首字符不同。
- 显式形参类型必须与期望函数类型逐位置**完全一致**（无变异性，与§7.4同规）。
- 零参lambda写作`() -> …`（括号不可省——省括号单参文法会与既有一元/二元表达式产生歧义，KISS不做）。

### 3.2 类型化

- lambda只在**期望函数类型的位置**成形（§7.4引用同策略：局部声明/初始化器/赋值/返回/实参/数组元素/花括号条目）。**字段初始化器除外**——其右值须为常量表达式且不得引用`this`（§4.8.3类/§4.7.2结构体），lambda不可能合法出现，`func`字段默认`null`。无期望类型的lambda是编译期错误，诊断点名lambda。
- 期望类型给出参数类型与返回类型；形参类型省略时按期望填充（复用集合初始化器已落地的`InferredTarget`通道，ExprResolverInitList.cpp:374-390）。
- 表达式体lambda的类型＝表达式类型；期望`void`（`func<void,…>`）而表达式体非void调用是编译期错误（不做静默丢弃，用块体写`f();`）。块体无`return`即void。

### 3.3 捕获语义

**按引用捕获**：lambda体内可见的外层局部变量（含形参）被捕获后，该变量获得堆存储，**同一变量的所有捕获点与外层读写共享同一存储**——lambda内外互相可见修改。依据§二共识；web回调/`thread.spawn`的可变共享状态是刚需，Java式final限制不采纳。

- **`this`捕获**：lambda体内访问实例成员（字段/方法）即捕获接收者；统一进display（见§四），实例方法内`this`非null由既有规则保证，静态上下文引用`this`维持编译错误。
- **`&`局部/`&`形参捕获**：捕获的是该引用（绑定）本身——按引用对`(堆句柄,单元偏移)`原样拷入display字段，lambda内的读写穿透到被引方存储。**生命周期安全由构造保证**：NLang的每个`&`绑定目标都是堆锚定存储——标量在绑定点即装箱提升（`NF_Promoted`，EmitExprRef.cpp:32-34的断言链：未提升标量到达发射＝内部不一致、响亮失败），结构/类局部本就是堆记录（`OP_RefOfLocal`），字段/数组/容器元素栖身堆记录。`foreach (int& v in xs)`的`&`头目标为容器元素（堆），同样安全。捕获不改变`&`的既有语义（§4.7.9）。
- **lambda的`&`形参与`&`返回**：`func<int&, int&>`期望下lambda可声明`&`形参、`&`返回——lambda降阶为合成类方法（§4.1），引用形参/引用返回在类方法上既有实现照搬（§4.7.9、§5.9）。
- **返回被捕获变量的引用合法**：被捕获变量的存储已堆化到display，§4.7.9「cannot return a reference to local; its storage ends with the call」的前提（存储随帧终结）不成立——`func<int&> f = () -> r;`放行。
- **循环头变量每次迭代新鲜**：`for`/`foreach`头部声明的变量，每次迭代是一个新鲜变量（Go 1.22共识）。未捕获时无可观察差异；捕获时各迭代的lambda各自持有当次值，不再共享尾值。新鲜性由display的分配模型直接给出（§4.1：循环体作用域每次迭代重新激活、重分配display）；头部声明的被捕获变量归属每迭代激活的display，条件段/递增段寻址当前迭代实例、迭代末经新display接续值链（Go 1.22实现形态）。存储口味随之分派：值变量接续拷贝槽位——标量按值拷贝，结构值头按记录深拷贝进新display的出生记录（按字段拷句柄会让全部迭代的闭包共享同一条记录、读尾值且写互通，发射走struct-aware字段存储；突变验证钉死）；`&`头（`foreach (int& v in xs)`）每迭代的pair各自绑定当次元素单元，写穿透到该单元；被提升的头（体内`&`绑定引发）每迭代换新盒，出迭代的闭包与引用持有旧盒、冻结在该迭代末值（for续接点读旧盒载荷、装新盒写入新display——按字段拷贝句柄会让全部闭包读到尾值，已用突变验证钉死）。
- **单语句循环体即块**：不带花括号的单语句循环体（含空语句`;`体）与包住该语句的花括号体完全同语义——文法在产出点统一包装成隐式块（主流读法：C#/Go/C++均将内嵌语句作用域归于循环）。体内声明的局部随迭代生效，体内捕获走上述同一套display机制；空语句体编译为空块（此前为编译器断言崩溃）。
- **static局部不被捕获**：lambda体内引用static局部直接寻址静态槽（模块级存储、所有调用共享，§6.1），与捕获机制无关。
- **递归自引用**：`func<int,int> f = (x) -> x <= 1 ? 1 : x * f(x - 1);`成立——被捕获的`f`在lambda创建前已提升，体内读`f`即读被捕获存储。
- 捕获不改变被捕获变量的声明语义：`const`（`NF_Const`）局部捕获后lambda内仍不可赋值。

### 3.4 值语义

- **闭包身份**：闭包值＝(lambda方法, display实例)对。display按作用域激活分配（§4.1）：同一lambda表达式在**同一次作用域激活**内求值两次得到**相等**的函数值（同一display、同一方法，C#语义）；**重新激活作用域**（再次调用含lambda的函数、循环下一迭代）得到**不相等**的闭包（新display）。零捕获同规（统一display路径，D3）——「!=」断言须用跨激活形态（工厂两次调用），同激活两求值恒相等。
- **相等**：函数值相等比较器从三元组`{target, receiver, form}`扩为四元组`{target, receiver, form, descIdx}`——同函数同实例化相等、不同实例化不等（§4.2解禁面自然延伸；「指向同一函数的两个引用相等」精神下descIdx必须参与，否则两个不同实例化的泛型函数值会假相等）。lambda闭包的receiver是display实例，`==`即闭包身份（同一display实例且同一lambda方法）。
- **toString**：渲染为`"lambda"`。判据＝函数表条目新增**lambda标志位**（`isGeneric`先例，随NCU函数表持久化），渲染点查标志位分流——不依赖合成名前缀约定。
- null、装箱、`Dict`键、`switch`判别式、序列化目标：func值既有禁则全部沿用（§7.4）。

### 3.5 编译期错误清单

lambda无期望类型；显式形参类型与期望不一致；`invalid lambda parameter list`（覆盖列表元素非裸标识符/类型形参形态）；lambda形参带默认值（文法无默认值位，点名诊断「a lambda parameter cannot have a default value」）；void期望配非void表达式体；lambda体内引用未声明名字；lambda带泛型参数表（`<T>`，本版非目标）；lambda内`out`标记（已退役诊断族）；静态上下文捕获`this`；lambda作字段初始化器（常量表达式规则不容）。

### 3.6 泛型上下文中的lambda

lambda定义在泛型函数/泛型类方法体内且捕获类型参数类型的变量（`func<void> make<T>(T x) { return () -> print(x); }`）**本版支持**：

- display合成类按**泛型类模板**合成——携带外层函数的全部类型参数，字段类型可为类型参数。实例化描述符**运行时拼接**：display的`OP_New`走TEMPLATE_HEAD＋ARG_CONTEXT组合行（机制既有，VmExecutorOpsDescriptor.cpp:30-36——组合记录行在运行时读执行帧的描述符单元拼出实例化；OP_New的descIdx操作数只认静态表行或0xFFFF哨兵、并无调用臂那样的转发臂，不能按字面复用外层实例化行）。
- 闭包值是泛型display实例上的绑定引用，描述符经接收者头填充（绑定委托既有通道，VmExecutorFrames.cpp:120-127）——`T`操作在lambda体内拿到真实描述符。
- 边界：泛型体内static局部禁令（§6.1）照旧——lambda体内（含其降阶体内）不得声明static局部，引用外层static局部走§3.3静态槽直达（不受泛型禁令的lambda侧放大）。

## 四、表示与降阶

### 4.1 统一display class降阶

lambda统一降阶为**编译器合成类**（display class）＋绑定引用：

- **display的分配粒度与时机**：display挂在**被捕获变量的声明作用域**上——每个这样的作用域激活一份，作用域进入时分配（该作用域内无lambda字面亦然；C#模型。兄弟块各自的lambda捕获同一外层变量因此共享同一display，存储不分叉）；零捕获lambda的display挂其所在作用域。lambda捕获多个作用域声明的变量时，其内层display持有对外层被捕获作用域display的引用（C#同构）。循环体作用域每次迭代重新激活——循环头变量的每迭代新鲜（§3.3、D2）由此直接给出。同一次激活内多次求值同一lambda共享该display，闭包相等；跨激活display不同，闭包不等（§3.4、D12）。
- 合成类字段＝全部被捕获变量（含`this`）；lambda体＝其实例方法；函数值＝`OP_MakeBoundFunc`（display实例为接收者）。**`&`引用对占两个display字段位**（基句柄槽＋偏移槽）——类字段寻址模型是每字段单个4字节值槽（VmExecutorGCMarks.cpp:50-51），8字节引用对无法单槽承载，拆双字段位最简。
- **零新增操作码**：绑定引用的「接收者随句柄走、状态跨调用保持」语义（§7.4）正是闭包语义；VM原语组合哲学（可组合原语优先）。
- **GC**：接收者追踪零改动（`MarkFuncReceiver`，VmExecutorGC.cpp:217）；display实例经句柄自然可达。**新增一处标记臂**：display的`&`捕获占基句柄＋偏移两个相邻字段位，`MarkClassFields`须标记基句柄槽（今天用户字段无`&`声明位，该臂为闭包新增）。
- 零捕获lambda同样走display（统一单条降阶路径，KISS）；无捕获缓存优化（C#形态C）非目标，语义不受影响。
- display类名编译器内部命名（不进用户可见诊断），对用户匿名。
- 4槽扩容的注释同步点：VmExecutor.h:2017（记录形状注释）、VmExecutorOpsCalls.cpp:385-392（Handle layout段）——实现期清单，防注释与记录形状漂移。

### 4.2 描述符配对存储（泛型函数值解禁）

- 函数值记录由3槽`{target, this, form}`扩为4槽`{target, this, form, descIdx}`（`AllocFuncRecord`）。记录是运行时内部形状，不入NCU线格式，非序列化面。
- `OP_MakeFunc`操作数追加`u16 descIdx`——哨兵沿用既有`kInstDescNone = 0xFFFF`（BytecodeOps.h:9，与OP_New同约定；0是合法描述符行）——**字节码格式变更，NCU版本提升**。绑定引用不增操作数：泛型类实例的描述符照旧从头描述符通道取（0.13.1a既有）；全部`OP_MakeBoundFunc`路径（含lambda闭包）的记录descIdx槽恒填0xFFFF——描述符经接收者头，槽位对绑定形式无独立意义，§3.4四元组相等的descIdx维度在闭包上由此恒等。
- 解禁面：泛型游离函数引用`T max<T>(T a, T b)`赋给`func<int,int,int>`——类型实参由期望类型推断，实例化描述符随句柄配对；泛型静态方法引用同通道。`CallDelegateFree`的`isGeneric`拒绝（VmExecutorOpsCalls.cpp:338）改为从句柄descIdx填充被叫帧描述符单元。
- 0.13.1a的具名拒绝诊断退役，按§3.5新诊断族替换。拒绝位点两处（计划评审M2修正）：resolver侧泛型类方法臂文本为"a function of a generic class cannot be referenced as a value"（ExprResolverFuncRef.cpp:82-85）；泛型游离函数值引用的resolver拒绝位点实现期实测钉位（RejectFuncRefTarget四处gate未见isGeneric检查）。VM防御后stop（"a generic function cannot be referenced as a value"，VmExecutorOpsCalls.cpp）随descIdx解禁收窄为「描述符缺失才拒」。

### 4.3 ABI与版本

- NCU v2.23，变更两处：`OP_MakeFunc`操作数形状；函数表条目lambda标志位（toString判据，§3.4）。反汇编、链接重映射、nloader/nlink校验和随格式位联动。
- 门禁：三树（master/master-asan/master-ide）＋docs五腿；e2e清单新增lambda族与泛型函数值族（见§五）。

## 五、测试与验收

- **e2e—lambda族**（真实编译＋VM执行）：计数器跨调用保持（lambda内外互见修改）；两个lambda共享同一捕获存储（f1改f2见）；循环捕获每迭代新鲜（for与foreach各一，断言非尾值）；递归lambda；`thread.spawn(() -> …)`零参lambda；lambda入`List<func<…>>`与`contains`/`indexOf`；`toString`=="lambda"；同一次作用域激活内同一lambda两求值`==`、两次进入作用域（工厂函数两次调用）`!=`；`&`捕获逃逸后调用（spawn回收写累加）；返回被捕获变量的引用；泛型体内lambda捕获`T`变量（实例化后`T`操作正确）。
- **e2e—泛型函数值族**（与捕获无关的解禁验收）：泛型游离函数值赋给具`func`类型后调用；泛型静态方法引用；两个不同实例化的句柄`!=`、同源两引用`==`。
- **单元**：文法（`->`与`==`/`>=`无冲突、覆盖文法`(x)`/`(x,y)`/`(int x)`分流、块体/表达式体）；类型化（期望类型推断、显式不一致、void丢弃拒绝）；诊断文本逐字断言（含`invalid lambda parameter list`、默认值形参）；降阶产物形状（display字段集、MakeBoundFunc位点、byref字段标记臂）。
- **手册同步点**：§7.4整节（lambda语法/捕获/相等/toString/错误清单）、§2文法概览、§5.9函数值表达式（lambda语法落点＋引用对照）、§6.1 static局部与lambda、§26 NCU格式（v2.23、MakeFunc操作数、函数表标志位）、`stdlib/http.n:1358`注释（「The language has no anonymous function literals」失实，随web批或本版一并改）、§18 Promise页「直接派生带返回值的委托当前要求先经显式委托局部」表述（`thread.spawn(() -> …)`改写）。

## 六、非目标

- 泛型lambda（`<T>`参数表）；lambda方法引用语法（`::`）；无捕获缓存/单例优化；闭包序列化；匿名结构化函数糖（Kotlin式尾lambda/ITK）；编译器生成名对用户可见（调试器显示归IDE线）。

## 七、裁决记录（D表）

| # | 裁决 | 依据 | 代价 |
| --- | --- | --- | --- |
| D1 | 按引用捕获（堆存储、全捕获点共享） | C#/Go/Kotlin共识；web/spawn可变共享刚需 | 实现需变量提升，编译器侧工作量 |
| D2 | 循环头变量每次迭代新鲜（display每迭代重分配直接给出） | Go 1.22/C#5共识，防共享尾值陷阱 | 循环体作用域每迭代激活一份display |
| D3 | 统一display class降阶、零捕获同路径 | 单路径KISS；语义与优化正交 | 含lambda作用域每次激活分配display（未来可优化） |
| D4 | 函数值记录3→4槽配对descIdx，MakeFunc加操作数，哨兵0xFFFF | 绑定引用描述符照旧走对象头，仅无接收者路径缺源；配对存储是挂账既定方向；0xFFFF与OP_New/调用臂同约定，0是合法描述符行 | NCU版本提升一次；无向下兼容负担（既定原则） |
| D5 | toString渲染`"lambda"`，判据＝函数表lambda标志位 | 匿名无名字；防合成名泄漏；`isGeneric`先例可持久化 | 函数表加一位，随NCU走 |
| D6 | 表达式体void期望非void＝编译错误 | 类型诚实，无静默丢弃 | 用户写块体，成本一行 |
| D7 | 省略形参类型按期望填充（复用InferredTarget通道）、显式必须全同 | §7.4无变异性同规；推断面最小、通道已有 | — |
| D8 | 零参括号强制 | 省括号与表达式文法歧义，KISS | — |
| D9 | 覆盖文法：内层统一表达式列表、`)`处lookahead `->`分流、参数形态`->`后语义后检 | `(x)`括号表达式与单参前缀在LALR(1)内不可两全；`->`不入任何表达式FOLLOW集故shift决策无冲突；`(int x)`类型关键字自然分流；自定义类型名`(Point p)`中ident后跟ident在表达式文法中无去处、只有`TypeRef TT_Identifier`路径可活，同样分流；本项目避LALR冲突改设计有先例（`as`弃`(T)`前缀cast） | 语义层多一道参数形态后检 |
| D10 | 泛型上下文lambda支持：display按泛型类模板合成、实例化随外层、描述符经接收者头 | 与泛型函数值解禁同版相邻，交互不可回避；全部原语既有（OP_New descIdx、绑定头通道），VM组合哲学；排除须独立理由而工作量论不成立 | display合成参与实例化机制，编译器侧复杂度 |
| D11 | `&`捕获安全（按构造堆锚定），GC补byref字段标记臂；返回被捕获变量引用放行 | 提升机制实证（NF_Promoted断言链）；「存储随帧终结」前提对display不成立 | MarkClassFields新增一臂；引用对拆基句柄＋偏移两字段位 |
| D12 | 闭包身份＝(lambda方法, display实例)；display按作用域激活分配、作用域进入时创建，同激活内两求值相等、跨激活不等（C#语义） | 按引用捕获迫使被捕获变量存储唯一＝该激活一份display，「每次求值新闭包」与「共享存储/同一性相等」三联不可同时成立（第二轮R2-I1）；相等性跟随身份最简 | 与零捕获统一路径；e2e「!=」断言须用跨激活形态（工厂两次调用） |
