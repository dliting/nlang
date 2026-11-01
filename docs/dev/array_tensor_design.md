# 数组/张量统一设计

日期：2026-11-01
状态：已定稿（概要设计，v2）
修订记录：v1提出静态/动态两层并存+视图只进动态层；对抗审查（编译器专家与程序员双视角）推翻两层骨架，v2改为单一类型模型，并按用户四项裁定补入slice（`:`语法）、广播（本轮）、单类型接口优先与机器学习模型验收标准。二轮审查后按用户"便利优先"裁定，混用降轴、负步长、负端点、轴置换、full重载与-1推断六项提前（D16）；负标量下标裁定支持、实现靠后（D14）。

## 1 背景与目标

NLang是类型安全的脚本语言。数组与张量的最终设计裁定（2026-11-01，用户定案）：

1. **单一类型模型**：`Array<T>`是唯一数组类型；`int[]`/`int[,]`是同一类型的静态秩标注（拼写层面的承诺），不是另一个类型。接口合理性优先，不做为保护快速路径而牺牲接口的优化（物化规则已删）。
2. **静态秩的检查价值保留**：秩承诺在边界检查——值流入带标注的位置时编译器插入隐式运行期秩检查；标注内部仍编译期查坐标数。检查摊销到边界，不在每次访问。
3. **视图语义**：存储与视图拆分（O(1)reshape/transpose/slice），视图记录与拥有记录统一处理，视图深度一。
4. **slice本轮纳入**：`a[1:3]`numpy式`:`语法（文法已核验安全），返回视图。
5. **广播本轮纳入**：numpy右对齐规则、标量操作数、零步长视图。
6. **数组进Object**：`Array<T> <: Object`，零分配上转型，协议方法按数组派发。
7. 不向下兼容，合理性优先；**验收标准：方便支持主流机器学习模型**，具体化为多层感知机训练程序端到端通过（见第8节）。

### 1.1 范围边界（本轮不做）

- 沿轴归约（`sum(a, axis)`）、批量matmul（前导维广播）、slice下标赋值（`a[1:3] = b`）——登记roadmap
- 负标量下标（`a[-1]`）——已裁定支持、实现靠后（牵连List/string下标一致性，需全局安排实现顺序），本轮不做
- Range类型（`..`为此保留，本轮不引入）
- 值泛型/const generics（`Array<int, 2>`写法，见决策D13）
- 按class机制重实现数组运行时表示（已否决，见决策D2）

## 2 调研参考

| 主题 | 结论 | 出处 |
|---|---|---|
| Rust const generics | `[T; N]`的N是类型的一部分（2021年1.51稳定）；但只覆盖一维长度，`N+1`等常量表达式**至今未稳定**——2026年10月官方博客提出Generic Const Arguments方案，明说"我们目前不准备稳定本文任何特性"。用const generics表达N维数组，Rust自己都没走通 | [Rust 1.51发布博客](https://blog.rust-lang.org/2021/03/25/Rust-1.51.0/)、[Generic Const Args and You](https://blog.rust-lang.org/inside-rust/2026/10/02/generic-const-args-and-you/) |
| Rust ndarray | `ArrayBase<S, D: Dimension>`，`Ix1..Ix6`静态秩、`IxDyn`动态秩，`.into_dyn()`显式转换；**静态秩层的形状值本身仍是运行期usize**——静态秩≠静态形状。生态讨论（PR #1568）指出维度抽象受元组限制最多12维 | [ndarray::IxDyn](https://docs.rs/ndarray/0.15.6/ndarray/type.IxDyn.html)、[Dimension系统剖析](https://deepwiki.com/rust-ndarray/ndarray/2.2-dimension-system)、[PR #1568](https://github.com/rust-ndarray/ndarray/pull/1568) |
| C# System.Array | 所有数组类型的抽象基类；`Rank`是运行期属性；`Array.CreateInstance`运行期定秩创建；秩错抛`RankException` | [System.Array](https://learn.microsoft.com/en-us/dotnet/api/system.array) |
| Julia | `Array{T,N}`秩作**值参数**，`N`必须单值，维度无关代码需`where {T,N}`；传错秩是运行期派发失败（MethodError）——"秩检查在边界"的先例。NLang类型实参只收类型，值参数路线走不通，改由静态秩标注+边界检查承担同等价值 | [Julia Arrays](https://docs.julialang.org/en/v1/base/arrays/) |
| numpy | 张量模型标准形态：存储块+视图头（dims/strides/offset）；reshape/转置/slice是O(1)视图；别名语义；**广播规则**：右对齐、维度相等或为1、结果取最大；切片端点越界截断、负端点与负步长合法（NLang采纳；标量负下标已裁定支持、实现靠后） | [NumPy internals](https://numpy.org/doc/stable/reference/internals.html)、[Broadcasting](https://numpy.org/doc/stable/user/basics.broadcasting.html) |
| JLS | 数组类型不是类类型，但`Object`是所有数组类型的直接超类型（§4.10.3）——"数组进Object"先例；Java数组`equals`是身份语义 | [JLS §10](https://docs.oracle.com/javase/specs/jls/se8/html/jls-10.html) |
| Go | 字符串进`any`走装箱，因字符串存储独立、句柄空间与堆索引冲突——NLang字符串进`Object`装箱的前轮裁定同因（数组上转型与字符串装箱并存不冲突） | NLang前轮设计裁定（见`docs/roadmap.md`） |

NLang侧代码事实（全部实测核验）：

- 数组记录布局：`[0]=RTK_Array, [1]=arrayTypeIdx, [2]=rank, [3]=elemCount, [4..4+rank-1]=dims`，载荷起点`ArrayHeaderCells(rank)`补到偶数格（`CompiledModule.h:167-179`）
- 元素宽度按种类：8字节标量族2格、其余1格（`ArrayElemCells`，同文件207-211；206行"cell 3 + i*cells"是陈旧注释，本设计一并修正）
- 静态下标op携带秩立即数，与记录秩不符抛`std::runtime_error`（`VmExecutorOpsArrayN.cpp:56-59,105-107`）——v2中该比对降级为边界检查的兜底，边界检查本身抛可捕获异常
- `OP_ArrayShape`/`OP_ArrayLen`运行期读`slot[2]`——记录本就每实例带秩，变秩不是表示层工程
- 数组方法（length/rank/shape/toString）是编译期静态发射，零intrinsic行（`BindingTable.cpp:129-165`）
- `Array`面类在类型位被拒（`ExprResolverBuiltinClasses.cpp:99`），声明在`builtins.n`
- 擦除式泛型：一个模板行，backing class以裸类名注册一次（`TypeDesc.cpp:314-318`，List/Dict同机制）
- 类下转型：`OpCheckCast`走超类链，失败抛可捕获`InvalidCastException`；**null抛宿主级不可捕获错误**（现存瑕疵，`VmExecutorOpsObjects.cpp:153-199`，本设计顺手统一）
- `ReceiverClassIndex`的RTK_Boxed臂扫描裸名"Object"（同文件235-248）——协议派发加数组臂的先例
- 函数无类型参数（`TypeParamsOpt`只挂`ClassDecl`，`nlang.y:1551+`）——tensor内核的元素种类轴无法用泛型函数折叠，4条是文法约束下的最小形态
- tensor包reshape族32条原生声明，实现为整块复制（`tensor_matrix.cpp:112-117`）
- **slice文法取证**：`SubscriptExpr`产生式是`Expression '[' Expression ']'`与`Expression '[' ConcreteParamList ']'`（`nlang.y:2265-2272`），下标内部不含键值冒号；键值冒号`InitEntry: Expression ':' Expression`只活在字面量`{...}`/`[...]`（2165行起）——`:`在`a[...]`内完全空闲，slice用`:`无文法冲突、词法零改动
- jagged拒绝：一个类型只带一组方括号（`array.md:10-16`）；`List<int[]>`放行但`List<int[][]>`拒绝（`generics.md:137-143`）

## 3 总体设计

```
        Object
          ↑ 隐式派生
       Array<T>            ← 唯一数组类型（backing行照List/Dict注册）
          ↑ 同一类型，两种拼写
   int[] / int[,] / ...    ← 静态秩标注（拼写承诺，非另一个类型）
```

**类型层**：`Array<T>`是唯一数组类型。`int[]`/`int[,]`是对该类型的静态秩标注——声明处写几个逗号，编译器就在该值上承诺秩为几：内部编译期查坐标数、发射秩立即数快速指令；值流入带标注的位置（赋值、参数、返回）时插入隐式运行期秩检查。**没有转换矩阵**——同一类型，赋值双向自由。

**值层**：两种记录形状，所有数组op统一认识（一个分支）：

- **拥有记录**：现状字节级不变（头部+载荷内联）
- **视图记录**：头部前缀与拥有记录**逐字段同义**（rank/elemCount/dims在同偏移），尾部追加strides/offset/target；恒直指拥有记录（深度一，组合时折叠）

**语义层**：reshape/transpose/slice返回视图（别名语义，numpy同构；非连续源reshape物化）；广播走零步长视图机制；协议方法与Object集成照v1。

## 4 分项设计

### 4.1 类型层

**声明**：`builtins.n`的`Array`面类升格为泛型类`class Array<T>`，方法面在现有四个方法上增补`equals`/`getHashCode`声明（原生，身份语义）。裸`Array`在类型位继续拒绝，`Array<int>`是泛型实例化，放行。

**静态秩标注**：方括号拼写（`E[]`、`E[,]`、`E[,,]`…，秩=逗号数+1）是`Array<E>`的标注形式，类型系统中两者是**同一类型**。标注的承诺由编译器兑现：

- 标注内部：坐标数编译期检查（既有诊断"expects %u indices"保留）、`rank()`折叠为常量、下标发射秩立即数指令
- 边界：值流入标注位置时插入隐式秩检查op，失败抛可捕获`InvalidCastException`（消息带预期/实际秩）。检查点=赋值、参数传递、返回——每个边界一次，不在每次访问
- 显式拼法：`a as int[,]`保留，与隐式检查同一运行期op（自文档化用途）
- null通过返回null（Java/C#语义）；**现存瑕疵**：类下转型`OpCheckCast`对null抛宿主级不可捕获错误（`VmExecutorOpsObjects.cpp:157-159`），与本设计null语义矛盾，顺手统一（独立小修复，随Step 1交付）

**子类型关系**：`Array<E> <: Object`。不变量：`Array<int>`与`Array<long>`无关；`Array<Object>`不对`Array<int>`拓宽（防写穿，与`List<T>`不变量一致）。

**`Object` ↔ 数组**：`Array<E>`→`Object`隐式上转型**零分配**（区别于字符串的装箱路线`TCK_Box`）；`Object`→`Array<E>`用`as`，运行期校验槽位kind为拥有记录或视图记录且元素种类匹配，失败抛可捕获`InvalidCastException`。视图与拥有记录同权：`Object o = a[1:3];`、`o as int[]`同样成立，协议派发臂两种kind都认。

### 4.2 运行时记录

**拥有记录**：字节级不变。

**视图记录**（新kind `RTK_ArrayView`，`CompiledModuleRecords.h`）：

```
[0]=RTK_ArrayView
[1]=arrayTypeIdx          ← 与拥有记录同偏移同义
[2]=rank                  ← 同偏移同义
[3]=elemCount             ← 同偏移同义
[4..4+rank-1]=dims        ← 同偏移同义
[4+rank..]=strides（rank项，0=广播维，负=逆向轴）
[4+2*rank]=offset（相对target载荷起点的元素偏移）
[4+2*rank+1]=target（恒为拥有记录——深度一）
```

**共享头部前缀**是该布局的核心：rank/elemCount/dims在同偏移，使所有元数据op（`OP_ArrayLen`、`OP_ArrayShape`、秩检查、`toString`格式化、foreach长度）天然形状无关、零分支；只有元素访问（load/store）按kind分支。深度一保证访问沿target只跳一次。

**创建校验**：rank在1..`kMaxArrayRank`(64)、dims非负、elemCount≤`kMaxArrayElems`(100M)、视图覆盖范围（最小与最大元素地址）落在target边界内、arrayTypeIdx与target一致。失败抛与创建同族的可捕获异常。

**GC**：视图记录恒被追踪（target是引用边），target按既有`ArrayElemKindAccepts`的elemKind剪枝——元素追踪逻辑原封不动留在拥有记录上。拥有记录被活视图引住即存活：既有标记可达性语义直接成立，无新增机制。

### 4.3 下标、slice、创建与遍历

**动态下标**：`a[i, j]`在`Array<T>`拼写下同样写法。编译器仍知坐标数（源码写定），帧槽位摆法与静态op相同，**无坐标数组分配**。动态load/store op运行期比对坐标数与记录秩，不匹配抛可捕获`IndexOutOfBoundsException`（消息含两个秩）；元素地址=拥有记录时行主序`flat = flat*dims[d]+idx`，视图记录时`offset + Σ idx[d]*stride[d]`经target取载荷。

**slice**（`:`语法，numpy式，文法已核验无冲突）：

- 形式：`a[start:stop]`、`a[start:stop:step]`、`a[:]`（整轴）、`a[:stop]`、`a[start:]`、`a[::-1]`（逆向）；多轴组合`a[1:3, :]`；**下标各轴可为slice项或标量下标**——含任一slice项结果恒为视图，全标量结果恒为元素
- 端点与步长：端点可为负（自末尾起算，`len+neg`），越界截断（numpy同）；`step`非零、可负（负步长逆向取轴，缺省端点翻转：start=len-1、stop取-1哨兵即含0——numpy同，用户显式`-1`按`len+neg`换算为len-1，与缺省哨兵不同，实施计划须钉死此陷阱）；长度正步长`(stop-start+step-1)/step`、负步长`(start-stop-step-1)/(-step)`；`start==stop`得零长维度（合法）
- **秩与降轴**（numpy同）：结果秩=slice轴数（编译期知）；标量轴索引折叠进offset、该轴从dims/strides剔除——`a[i, :]`得秩1行视图、`a[:, j]`得秩1列视图、`a[i, j]`仍是元素；标注拼写下结果标注=新秩，边界检查自然衔接
- 组合折叠：slice的slice、slice的transpose等一律折回拥有记录（深度一）；reshape/flatten对非连续视图（逆向、转置、降轴）**物化**为拥有记录（numpy同，reshape要求行主序可重排）
- 下标赋值`a[1:3] = b`：本轮范围外（登记roadmap），期间可经循环逐元素写

**创建**：动态构造沿用泛型构造的既有形式——裸模板名对声明目标推断实例化：`Array<int> a = Array(dims);`（`dims`为`int[]`，秩=`dims.length()`运行期校验，见generics 4.12.3的`Box<int> b = Box(7)`先例）；静态拼写`T[d1, d2]`保留。

**foreach**：既有foreach走线性索引（rank-1与rank-N同址，`VmExecutorOpsArrayN.cpp:153`注释），`Array<T>`上沿用——按elemCount平坦遍历、元素类型T；视图记录经动态op把线性索引换算坐标（除模dims）再按strides取址。

### 4.4 广播

- 规则：numpy右对齐——逐维比较，相等或其一为1则相容，结果维取最大；不相容抛可捕获异常（消息带两个形状，沿用tensor既有消息风格）
- 覆盖：逐元素族（add/sub/mul/div）的数组-数组与数组-标量两种形态（标量=零维广播，偏置加法的基础）；`broadcastTo(Array<E>, int[] shape)`显式视图接口（零步长视图，O(1)）
- 机制：视图记录允许零步长（第4.2节）；内核按有效(dims, strides, offset)读取操作数，广播维步长记0，循环按广播形状展开
- matmul：本轮保持秩2要求（运行期校验，消息同既有）；批量matmul（前导维广播）登记roadmap

### 4.5 错误模型分档

| 错误 | 类别 | 处理 |
|---|---|---|
| 边界秩检查失败（标注位置流入错秩值） | 正常用户错误 | 可捕获`InvalidCastException`（消息带预期/实际秩） |
| 坐标数与秩不符（动态下标） | 正常用户错误 | 可捕获`IndexOutOfBoundsException` |
| 广播形状不相容 | 正常用户错误 | 可捕获（消息带两个形状） |
| 形状参数非法（reshape元素数不符、-1推断不可整除、transpose axes非排列、slice step为0） | 正常用户错误 | 可捕获（消息沿用tensor既有形状） |
| 编译器不变量破坏（标注内部op秩立即数与记录不符等） | 编译器缺陷 | 宿主`std::runtime_error`不可捕获（现状保留，边界检查兜底在前） |
| 视图创建校验失败 | 正常用户错误 | 可捕获（同创建族） |

### 4.6 tensor包面（机器学习基本面）

rank轴由`Array<T>`承担，元素种类轴因函数无类型参数保留4条；静态秩旧名（reshape1..8等）**删除**（不向下兼容）。每个函数4条原生声明（int/long/float/double）：

- 创建：`full(int n, T v)`与`full(int[] dims, T v)`（重载）、`arange(...)`（既有）；零填充直接用构造`Array(dims)`（语言内建，无需zeros）
- 形状：`reshape(Array<T>, int[] shape)`（视图；shape允许一个`-1`推断维，非连续源物化）、`flatten(Array<T>)`（视图，等价reshape到秩1）、`transpose(Array<T>)`（视图，轴全反转）与`transpose(Array<T>, int[] axes)`（视图，任意轴置换，axes须为0..rank-1的排列）、`slice(Array<T>, int[] starts, int[] stops, int[] steps)`（函数形式，全轴slice化，端点与步长语义同下标slice）、`broadcastTo(Array<T>, int[] shape)`（视图）
- 逐元素：`add/sub/mul/div(Array<T>, Array<T>)`与`add/sub/mul/div(Array<T>, T)`（广播）；`max/min(Array<T>, T)`（元素级截断，relu基础）；`exp/log(Array<T>)`（激活、softmax与交叉熵基础）
- 归约：`sum/max/min(Array<T>)`（全张量；沿轴归约登记roadmap）
- 结构：`matmul(Array<T>, Array<T>)`（秩2校验）、`concat(Array<T>, Array<T>, int axis)`（复制）
- 配套stdlib：`random.n`——带种子伪随机（`seed(int)`、`nextFloat()`、`nextInt(int n)`），nlang实现的线性同余生成器（语言整型环绕规则自带取模），供权重初始化与数据打乱
- 组合层以nlang实现示范：`relu(a) = max(a, 0)`、`softmax(a) = exp(a) / sum(exp(a))`、交叉熵`loss = -log(softmax[y])`（纯nlang代码）——体现"高层特性归应用层"

### 4.7 native ABI与外围

- `NativeArrayView`（C ABI）增补strides/offset，ABI版本v8→v9
- 逐元素/归约内核：步长感知循环（含零步长广播维与负步长逆向维，寻址与符号无关）；matmul：连续检查+非连续物化（转置/负步长视图走物化路径）
- 序列化：视图按逻辑内容物化序列化（numpy pickle同语义）；反序列化恒产出拥有记录
- 调试器：数组显示读记录处需认识`RTK_ArrayView`（显示dims/strides/offset，元素沿target解析）
- 陈旧注释修正：`CompiledModule.h:206`"cell 3 + i*cells"改为指向`ArrayHeaderCells(rank)`
- 面类解析：静态数组类型的方法查找现走`FindPreludeClassDecl("Array")`（`ExprResolverMemberBuiltins.cpp`）；面类变泛型后，该查找改为对模板行做方法匹配（native方法元数即可，无需T）

## 5 决策理由

- **D1 单一类型模型，方括号为静态秩标注**：v1的两层（独立类型+转换矩阵+物化规则）在对抗审查中三处失守——物化是隐藏O(n)（同一行代码的复杂度取决于不可见历史）；"静态层永不遇视图"靠native契约而非机制；两套类型是永久税而静态层零新功能。单类型下秩承诺改为边界检查：检查价值（坐标数编译错、`rank()`常量折叠、秩立即数指令）全部保留，且无转换矩阵、无物化、无契约。Julia的`Array{T,N}`（秩在类型、边界派发检查）是先例；接口合理性优先、避免过早优化是用户裁定。
- **D2 运行时表示维持RTK_Array，不按class重实现**：前轮已证"class记录装不下变长"的说法错误（`StructSlot`变长记录与数组共存于同一对象堆）。真实代价是两条：元素宽度规则冲突（字段统一8字节步长vs数组按种类4/8）与记录消费者横切特例；收益（声明面+虚派发）已被面类与`Array<T>`真类型拿到。.NET对String的变长尾是同类特例，证明可行而非值得。
- **D3 动态层命名`Array<T>`而非`Tensor<T>`**：与既有`Array`面类连续（零孤儿迁移）、与roadmap"Array升格"一脉相承；`Tensor`名承诺视图语义会造成名字与语义的错位期（tensor包仍叫tensor，类型名不占"张量"一词）。
- **D4 slice用`:`而非`..`**：文法取证——下标内部（`SubscriptExpr`产生式）不含键值冒号，`:`在`a[...]`内完全空闲；键值冒号只活在字面量`{...}`/`[...]`（`nlang.y:2165`起），两上下文不相交。numpy习惯、少一字符；`..`为将来Range类型保留（Rust先例）。用户裁定。
- **D5 视图共享头部前缀**：rank/elemCount/dims与拥有记录同偏移同义，元数据op天然形状无关；只有元素访问分支。深度一（视图恒指拥有记录，组合折叠）使访问与GC不递归，numpy同构。
- **D6 边界秩检查隐式插入**：同一类型无"转换"可言，标注是承诺不是类型；检查失败是用户错误，抛可捕获异常合理（Julia派发失败先例）。`as int[,]`保留为显式拼法（自文档化）。不向下兼容裁定下，`int[,] m = 动态值;`直接可用且可失败——方便优先。
- **D7 广播本轮纳入**：numpy规则是事实标准；标量广播是ML偏置加法的基础；零步长视图机制成本约等于零（记录校验加一条允许零的规则）。matmul保持秩2（批量matmul登记roadmap），KISS。
- **D8 数组进Object**：落实roadmap①，消除`List<Object>.add(int[])`不一致；上转型零分配（区别于字符串装箱）；协议派发沿RTK_Boxed臂既有先例（`VmExecutorOpsObjects.cpp:235-248`）。
- **D9 协议方法身份语义**：与Object默认、Dict身份键、Java/C#数组Equals一致；内容相等留给将来设计。
- **D10 下标同形+运行期坐标数校验**：语言内数组下标不应有两套语法；静态拼写下保持编译期检查与秩立即数指令，动态拼写下把静态op里的"不可能发生的断言"降级为"可能发生的检查"（`VmExecutorOpsArrayN.cpp:56-59`那条比对），无分配无额外间接。
- **D11 tensor秩轴塌缩、元素种类轴保留**：rank轴由`Array<T>`承担；元素种类轴因函数无类型参数而保留4条，是文法约束下的最小形态。
- **D12 jagged继续拒绝**：`Array<Array<int>>`与`int[][]`（两组方括号）是数组的数组，触犯"无jagged布局"不变量（`ExprResolverTypeArgs.cpp:16`）；`List<Array<int>>`与`Array<List<int>>`放行（容器装数组引用已有`List<int[]>`先例）。
- **D13 不做const generics**：`Array<int, 2>`需要值泛型，NLang类型实参只收类型（文法）；Rust实证——`N+1`十五年后仍未稳定（2026年10月官方博客明言不准备稳定），此路成本与收益严重不成比例。Julia的`Array{T,N}`因此不可照抄，静态秩标注+边界检查承担同等价值。
- **D14 沿轴归约、批量matmul、slice下标赋值登记roadmap；负标量下标已裁定支持、实现靠后（用户2026-11-01）**：前两者有真内核成本（任意轴kernel族、前导维广播语义）；slice下标赋值要视图左值机制；负标量下标（`a[-1]`=`len+neg`换算）已裁定支持，因牵连List/string下标一致性，实现顺序靠后，随roadmap登记。
- **D15 验收数据集用公开数据打包子集（用户裁定）**：合成数据可验证性不足（损失下降对破损实现也可能成立，无法对拍）。取MNIST——多层感知机是MNIST的主流体基线模型（CIFAR的主流基线是卷积网络，用MLP跑CIFAR反而不主流），且体量小、3分钟时限余量大。打包子集+官方提取脚本+校验和，兼顾离线测试与可复现验证；同法可换CIFAR（仅基线数值不同）。程序命名`simple_mlp.n`——将来可能有多个MLP相关程序。
- **D16 便利优先裁定（用户2026-11-01：不向下兼容、低成本前提下程序员便利优先）**：六项由roadmap提前——①slice与标量下标混用（降轴=offset折叠+轴剔除，视图构造零新机制）；②负步长slice（stride带符号是视图布局天然能力，逐元素与foreach寻址与符号无关）；③负端点（len+neg一条换算，slice是新语法不触碰既有标量下标规则）；④transpose轴置换参数（dims/strides重排，O(rank)）；⑤`full(int[] dims, T v)`重载；⑥reshape`-1`推断维（一次除法的量级）。仍延后者有真成本，见D14。

## 6 实施步骤

### Step 1 类型层（无视图）

1. `builtins.n`：`Array`面类升格为`class Array<T>`，增补`equals`/`getHashCode`原生声明
2. 编译器：`Array<int>`放行、方括号=静态秩标注（类型同一、标注随节点）、边界秩检查op插入（赋值/参数/返回）、`as int[,]`显式拼法、null通过
3. 运行时：动态load/store op一对（拥有记录路径，行主序）；`ReceiverClassIndex`数组臂；backing行注册（照List/Dict）
4. 修`OpCheckCast`null瑕疵（4.1节）
5. tensor：32→4声明塌缩（reshape仍复制，Step 2换视图）；新增`random.n`带种子伪随机包（nlang实现）；relu/softmax/交叉熵的nlang示范
6. 文档同步（见第7节）、测试（真实编译执行）

### Step 2 视图、slice与广播

1. `RTK_ArrayView`kind与视图记录（共享头部前缀+尾部strides/offset/target、深度一）、视图创建native（`NativeHost`扩展）、创建校验（含零步长）
2. 下标op双形状处理（静态N系与动态系都加视图分支）、foreach视图路径
3. GC视图边（target追踪）
4. ABI v9（strides/offset）、逐元素/归约内核步长化（含广播维与负步长）、matmul连续检查
5. reshape/transpose/slice改视图实现；slice语法（下标内`start:stop`与`start:stop:step`产生式、词法零改动）
6. 广播：右对齐规则校验、标量形态、`broadcastTo`
7. 序列化视图物化、调试器认识视图
8. 测试（含别名互写可见、GC压测视图挂住target、广播形状矩阵、序列化往返）

### Step 3 验收与收尾

1. **验收：`examples/simple_mlp.n`训练程序**（第8节：MNIST打包子集+`tools/`提取脚本+校验和）作为端到端测试并入测试集，准确率基线实测后冻结
2. roadmap更新：远期特性改写为单一类型模型表述（含修正已提交否决理由中"class记录字段数按类型固定"的错误半句）；登记D14的roadmap项（负标量下标标注"已裁定支持"）
3. 手册规格页（en+zh）终校、VERSION提升、CHANGELOG记录

## 7 影响面与文档同步清单

- 运行时/编译器：`CompiledModuleRecords.h`、`CompiledModule.h`（视图头常量+陈旧注释）、`VmExecutorAlloc.cpp`、`VmExecutorOpsArrayN.cpp`、`VmExecutorGC.cpp`、`VmExecutorOpsObjects.cpp`、`CastInfo.cpp`（array→Object）、`ExprResolverSubscript.cpp`（slice产生式+动态下标）、`ExprResolverTypeArgs.cpp`、`ExprResolverMemberBuiltins.cpp`、`ExprResolverBuiltinClasses.cpp`、`BuiltinNames.h`、`NativeHost.h`、`src/native/tensor/*`、`src/compiler/grammar/nlang.y`（slice产生式）
- 标准库：`stdlib/builtins.n`、`stdlib/tensor.n`、`stdlib/random.n`（新增）
- 文档：`docs/user_manual/en/specification/04-types/{array,generics,object,casts}.md`及对应中文页（`Array<T>`唯一类型、静态秩标注语义、slice、广播、Object路径、视图与别名语义）、`docs/roadmap.md`、`README.md`
- 其他：ABI版本v8→v9、调试器数组显示、序列化、`VERSION`、`CHANGELOG.md`

## 8 验证要点与验收标准

**功能验证**：

- `List<Object>.add(int[3])`可用，`get`后`as int[]`取回
- `Object o = int[2,2];`零分配（GC统计无新增记录）；`o.toString()`按数组格式
- `Array<int> a = int[2,3]; a[0,1]`读写正确；坐标数错抛可捕获`IndexOutOfBoundsException`
- 边界检查：`int[,] m = a;`（a实际秩3）抛可捕获`InvalidCastException`（消息带秩3/2）；标注内部`m[1]`仍是编译错
- slice：`int[,] m; m[1:3, :]`返回视图、`m[i, :]`行视图（标注秩1）、越界端点截断、`a[:]`整轴、`a[::-1]`逆向、`a[:-1]`负端点、`start:stop:step`步长；slice结果与源别名互写可见
- reshape/transpose O(1)：改视图dims/strides即完成；transpose axes置换视图、reshape`-1`推断维、非连续视图reshape物化；经视图写入对源可见
- 广播：`add(Array, 标量)`偏置、右对齐形状矩阵（`(3,4)+(4)`合法、`(3,4)+(3)`非法）、`broadcastTo`零步长
- 既有静态层语义逐字不变：数组测试全绿（含`int[,]`编译期坐标数检查）
- GC压测：视图存活时target不回收；视图与target同死一并回收；元素种类剪枝不受影响

**验收标准（用户裁定：方便支持主流机器学习模型，公开数据集可验证、不用合成数据）**：`examples/simple_mlp.n`——两层多层感知机（784→100→10）、手动反向传播、小批量随机梯度下降，在**MNIST官方数据集的打包子集**上训练与测试。数据落地形态：`tools/`提取脚本（官方下载地址、解析、校验和记录在案）+`examples/data/`快照，任何人可复现提取并比对。断言两项：训练损失随轮次下降；测试子集准确率≥基线（基线在程序落地时实测后冻结，写入程序注释）。程序必须只依赖tensor包与语言特性，覆盖第4.6节全表面：动态秩批量张量、matmul、偏置广播、relu（`max(a, 0)`）、softmax、交叉熵（`exp/log`）、sum归约、transpose、reshape、随机初始化。逐样本（秩1向量）前向与反向，规避本轮未做的沿轴归约；softmax按减最大值稳定化。该程序同时作为端到端测试常驻测试集；子集大小与训练轮次调优至运行时限3分钟以内（测试超时约束）。
