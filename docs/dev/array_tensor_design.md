# 数组/张量统一设计

日期：2026-11-01
状态：已定稿（概要设计，v3.2）
修订记录：v1提出静态/动态两层并存+视图只进动态层；对抗审查（编译器专家与程序员双视角）推翻两层骨架，v2改为单一类型模型，并按用户四项裁定补入slice（`:`语法）、广播（本轮）、单类型接口优先与机器学习模型验收标准。二轮审查后按用户"便利优先"裁定，混用降轴、负步长、负端点、轴置换、full重载与-1推断六项提前（D16）；负标量下标裁定支持、实现靠后（D14）。v3联网复核轮（一手文档取证）：裁定零步长视图只读（写穿抛可捕获错误，numpy readonly view同语义）、标注flatten命名对numpy的偏离（取ravel语义）、以Java协变数组ArrayStoreException与PyTorch负步长拷贝两处反例加固不变数组与负步长裁定、修正陈旧锚点（builtins.n已于0.12.5退役，`Array`面类真源为stdlib/lang.n；ABI实际已是v9——strides内联与newArrayShaped均已落地，本轮视图入口升v10）；事实核查发现数组内容序列化不存在，按用户「合理性优先、不向下兼容」裁定本轮补齐（writeObject/readObject数组值+对象图数组字段+视图物化，原「保持现状拒绝」的收缩撤销）。v3.1对抗审核轮（用户点名专抓「现状当设计依据」类问题，15项发现全处置）：撤销random.n（math面`srand/random/randomi`现成且确定性，新包三函数是严格子集——重复建设是序列化收缩的镜像病）；`sum(Array<T>, int axis)`移入本轮（步长内核建成后「任意轴kernel族」递延论据失效，验收程序改批量矩阵形态）；序列化实测更糟的静默腐蚀路径入档（裸数组writeObject把slot[0]当classIdx，类表>5项静默产垃圾）并钉定读取面`readArray()`自描述约定；视图尾部基址钉死4+rank不补偶；6处陈旧行号锚点按实测回填（OpCheckCast null臂214-216、ReceiverClassIndex 292起、SubscriptExpr 2324-2332、InitEntry 2370起、CompiledModule.h 178-190、OP_ArrayLen读slot[3]）；roadmap「字段数按类型固定」修正指令核销（本体已是修正后表述）。v3.2二轮对抗审核轮（5 Medium全处置）：①roadmap:109进行中条目仍记random.n与「沿轴归约登记远期」——指令已列入（改记math面复用、sum(axis)入本轮后远期项为max/min沿轴）；②readArray自描述tag对struct元素补identity（struct名入流，镜像类序列化名入流约定），§8往返矩阵加struct用例；③「实测」出产核正——裸数组writeObject的语言面现状是编译门拦截（`stream method "writeObject" expects a class argument`，2026-11-04实证），slot[0]当classIdx的腐蚀是门放行后的运行期路径（`VmExecutorSer.h`代码级核验），门的数组放行与kind分派须同批落地；④max/min沿轴递延补独立理由（本轮无消费者，`ReduceAxisKernel`模板落地后按需开启是一小步，KISS不预建无用面）；⑤§4.3哨兵措辞对齐编码事实（缺省哨兵编码INT32_MIN非语义值-1，显式INT32_MIN端点/步长与哨兵不可区分→归一化拒绝）；cast表复锚`src/compiler/CastInfo.cpp`（v3.1的「grep定位」过度泛化）。三轮审核收尾（0 Medium+，循环闭合）：「不足5项」改「不超过5项」（size==5同抛，`VmExecutorSer.h:226`为`>=`比较）。

## 1 背景与目标

NLang是一门类型安全语言。数组与张量的最终设计裁定（2026-11-01，用户定案）：

1. **单一类型模型**：`Array<T>`是唯一数组类型；`int[]`/`int[,]`是同一类型的静态秩标注（拼写层面的承诺），不是另一个类型。接口合理性优先，不做为保护快速路径而牺牲接口的优化（物化规则已删）。
2. **静态秩的检查价值保留**：秩承诺在边界检查——值流入带标注的位置时编译器插入隐式运行期秩检查；标注内部仍编译期查坐标数。检查摊销到边界，不在每次访问。
3. **视图语义**：存储与视图拆分（O(1)reshape/transpose/slice），视图记录与拥有记录统一处理，视图深度一。
4. **slice本轮纳入**：`a[1:3]`numpy式`:`语法（文法已核验安全），返回视图。
5. **广播本轮纳入**：numpy右对齐规则、标量操作数、零步长视图。
6. **数组进Object**：`Array<T> <: Object`，零分配上转型，协议方法按数组派发。
7. 不向下兼容，合理性优先；**验收标准：方便支持主流机器学习模型**，具体化为多层感知机训练程序端到端通过（见第8节）。

### 1.1 范围边界（本轮不做）

- 批量matmul（前导维广播）、slice下标赋值（`a[1:3] = b`）——登记roadmap；沿轴归约本轮纳入最小面`sum(Array<T>, int axis)`（对抗审核裁定2026-11-03：步长感知内核建成后它是同一泛型循环＋结果分配，「任意轴kernel族」的成本论据不再成立；批量MLP的梯度聚合直接需要。max/min沿轴仍roadmap）
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
| numpy视图三细节 | ①`broadcast_to`返回"**A readonly view** on the original array…more than one element of a broadcasted array may refer to a single memory location"——零步长别名使写语义未定义，numpy以只读封死（NLang同裁定，见4.2/4.4）；②`flatten`"Return **a copy** of the array collapsed into one dimension"而`ravel`"A copy is made **only if needed**"——NLang的flatten取ravel语义（视图优先、非连续物化），命名偏离须在手册标注；③`reshape`"This will be a new **view object if possible**; otherwise, it will be a copy"——与NLang设计一致 | [numpy.broadcast_to](https://numpy.org/doc/stable/reference/generated/numpy.broadcast_to.html)、[ndarray.flatten](https://numpy.org/doc/stable/reference/generated/numpy.ndarray.flatten.html)、[numpy.ravel](https://numpy.org/doc/stable/reference/generated/numpy.ravel.html)、[numpy.reshape](https://numpy.org/doc/stable/reference/generated/numpy.reshape.html) |
| PyTorch | `torch.flip`官方注记"**makes a copy** of input's data. This is different from NumPy's np.flip, which returns a view in constant time"——不支持负步长是PyTorch张量库的实现选择（flip被迫拷贝）；NLang按numpy路线（步长带符号、寻址与符号无关，D16②），内核须真正按步长寻址而非特判，此处是反例警示 | [torch.flip](https://pytorch.org/docs/stable/generated/torch.flip.html) |
| JLS | 数组类型不是类类型，但`Object`是所有数组类型的直接超类型（§4.10.3）——"数组进Object"先例；Java数组`equals`是身份语义；**协变反例**：Java数组协变（`String[] <: Object[]`），§10.5规定每次存储运行期检查——"If the type of the value being assigned is not assignment-compatible with the component type, an ArrayStoreException is thrown"（C#协变数组同型ArrayTypeMismatchException）。写穿税+运行期检查是NLang不变数组（`Array<E>`不协变，见4.1）的直接反例支撑 | [JLS §10](https://docs.oracle.com/javase/specs/jls/se8/html/jls-10.html) |
| Go | 字符串进`any`走装箱，因字符串存储独立、句柄空间与堆索引冲突——NLang字符串进`Object`装箱的前轮裁定同因（数组上转型与字符串装箱并存不冲突） | NLang前轮设计裁定（见`docs/roadmap.md`） |

NLang侧代码事实（全部实测核验）：

- 数组记录布局：`[0]=RTK_Array, [1]=arrayTypeIdx, [2]=rank, [3]=elemCount, [4..4+rank-1]=dims`，载荷起点`ArrayHeaderCells(rank)`补到偶数格（`CompiledModule.h:178-190`）
- 元素宽度按种类：8字节标量族2格、其余1格（`ArrayElemCells`，同文件218-222；217行"cell 3 + i*cells"是陈旧注释，本设计一并修正）
- 静态下标op携带秩立即数，与记录秩不符抛`std::runtime_error`（`VmExecutorOpsArrayN.cpp:56-59,105-107`）——v2中该比对降级为边界检查的兜底，边界检查本身抛可捕获异常
- `OP_ArrayShape`运行期读`slot[2]`（秩）、`OP_ArrayLen`读`slot[3]`（元素数）——记录本就每实例带秩与计数，变秩不是表示层工程
- 数组方法（length/rank/shape/toString）是编译期静态发射，零intrinsic行（`BindingTable.cpp:129-165`）
- `Array`面类在类型位被拒（`ExprResolverBuiltinClasses.cpp:99`），声明在`stdlib/lang.n:100`（builtins.n已于0.12.5退役）
- 擦除式泛型：一个模板行，backing class以裸类名注册一次（`TypeDesc.cpp:314-318`，List/Dict同机制）
- 类下转型：`OpCheckCast`走超类链，失败抛可捕获`InvalidCastException`；**null抛宿主级不可捕获错误**（现存瑕疵，null抛点在`VmExecutorOpsObjects.cpp:214-216`、by-design注释249-251，本设计顺手统一）
- `ReceiverClassIndex`的RTK_Boxed臂扫描裸名"Object"（同文件，函数在292起、Boxed臂292-303）——协议派发加数组臂的先例
- 函数无类型参数（`TypeParamsOpt`只挂`ClassDecl`，`nlang.y:1551+`）——tensor内核的元素种类轴无法用泛型函数折叠，4条是文法约束下的最小形态
- tensor包reshape族32条原生声明，实现为整块复制（`tensor_matrix.cpp:112-117`）
- **slice文法取证**：`SubscriptExpr`产生式是`Expression '[' Expression ']'`与`Expression '[' ConcreteParamList ']'`（`nlang.y:2324-2332`），下标内部不含键值冒号；键值冒号`InitEntry: Expression ':' Expression`只活在字面量`{...}`/`[...]`（2370行起）——`:`在`a[...]`内完全空闲，slice用`:`无文法冲突、词法零改动
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

**声明**：`stdlib/lang.n`的`Array`面类升格为泛型类`class Array<T>`，方法面在现有四个方法上增补`equals`/`getHashCode`声明（原生，身份语义）。裸`Array`在类型位继续拒绝，`Array<int>`是泛型实例化，放行。

**静态秩标注**：方括号拼写（`E[]`、`E[,]`、`E[,,]`…，秩=逗号数+1）是`Array<E>`的标注形式，类型系统中两者是**同一类型**。标注的承诺由编译器兑现：

- 标注内部：坐标数编译期检查（既有诊断"expects %u indices"保留）、`rank()`折叠为常量、下标发射秩立即数指令
- 边界：值流入标注位置时插入隐式秩检查op，失败抛可捕获`InvalidCastException`（消息带预期/实际秩）。检查点=赋值、参数传递、返回——每个边界一次，不在每次访问
- 显式拼法：`a as int[,]`保留，与隐式检查同一运行期op（自文档化用途）
- null通过返回null（Java/C#语义）；**现存瑕疵**：类下转型`OpCheckCast`对null抛宿主级不可捕获错误（`VmExecutorOpsObjects.cpp:214-216`），与本设计null语义矛盾，顺手统一（独立小修复，随Step 1交付）

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

（视图尾部基址＝`4+rank`，**不补偶**——`ArrayHeaderCells`的补偶只服务拥有记录的载荷对齐；视图尾部只有视图感知消费者读，普通载荷消费者被kind门槛拦在门外永不触尾，前缀同义性不受影响。）

**共享头部前缀**是该布局的核心：rank/elemCount/dims在同偏移，使所有元数据op（`OP_ArrayLen`、`OP_ArrayShape`、秩检查、`toString`格式化、foreach长度）天然形状无关、零分支；只有元素访问（load/store）按kind分支。深度一保证访问沿target只跳一次。

**创建校验**：rank在1..`kMaxArrayRank`(64)、dims非负、elemCount≤`kMaxArrayElems`(100M)、视图覆盖范围（最小与最大元素地址）落在target边界内、arrayTypeIdx与target一致。失败抛与创建同族的可捕获异常。

**零步长与只读**：strides允许0（广播维，`broadcastTo`产出）。**写穿零步长视图是可捕获错误**——一次逻辑写会命中同一存储位的多个逻辑位，写语义未定义；numpy以readonly view封死（broadcast_to返回"A readonly view…more than one element…may refer to a single memory location"，见第2节），NLang同语义但落在store op（视图任一stride为0即拒绝，消息指明须reshape物化得可写副本——零步长视图非连续，reshape本就物化）。读不受限：逐元素、归约、foreach、序列化（物化输出）照常。**负步长视图可写**（numpy同；PyTorch不支持负步长、其flip被迫拷贝，见第2节——NLang内核寻址与符号无关，两方向同权）。

**GC**：视图记录恒被追踪（target是引用边），target按既有`ArrayElemKindAccepts`的elemKind剪枝——元素追踪逻辑原封不动留在拥有记录上。拥有记录被活视图引住即存活：既有标记可达性语义直接成立，无新增机制。

### 4.3 下标、slice、创建与遍历

**动态下标**：`a[i, j]`在`Array<T>`拼写下同样写法。编译器仍知坐标数（源码写定），帧槽位摆法与静态op相同，**无坐标数组分配**。动态load/store op运行期比对坐标数与记录秩，不匹配抛可捕获`IndexOutOfBoundsException`（消息含两个秩）；元素地址=拥有记录时行主序`flat = flat*dims[d]+idx`，视图记录时`offset + Σ idx[d]*stride[d]`经target取载荷。store经视图记录时先查零步长（见4.2）：任一stride为0抛可捕获异常，其余视图（含负步长）照常写入。

**slice**（`:`语法，numpy式，文法已核验无冲突）：

- 形式：`a[start:stop]`、`a[start:stop:step]`、`a[:]`（整轴）、`a[:stop]`、`a[start:]`、`a[::-1]`（逆向）；多轴组合`a[1:3, :]`；**下标各轴可为slice项或标量下标**——含任一slice项结果恒为视图，全标量结果恒为元素
- 端点与步长：端点可为负（自末尾起算，`len+neg`），越界截断（numpy同）；`step`非零、可负（负步长逆向取轴，缺省端点翻转：start=len-1、stop哨兵=含0——**编码为INT32_MIN，非语义值-1**；numpy同，用户显式`-1`按`len+neg`换算为len-1，与缺省哨兵不同；**显式求值出INT32_MIN的端点/步长与哨兵不可区分，归一化拒绝**——可捕获「slice endpoint/step out of range」，显式存在位由发射侧伴随参数提供，见实施计划）；长度正步长`(stop-start+step-1)/step`、负步长`(start-stop-step-1)/(-step)`；`start==stop`得零长维度（合法）
- **秩与降轴**（numpy同）：结果秩=slice轴数（编译期知）；标量轴索引折叠进offset、该轴从dims/strides剔除——`a[i, :]`得秩1行视图、`a[:, j]`得秩1列视图、`a[i, j]`仍是元素；标注拼写下结果标注=新秩，边界检查自然衔接
- 组合折叠：slice的slice、slice的transpose等一律折回拥有记录（深度一）；reshape/flatten对非连续视图（逆向、转置、降轴）**物化**为拥有记录（numpy同，reshape要求行主序可重排）
- 下标赋值`a[1:3] = b`：本轮范围外（登记roadmap），期间可经循环逐元素写

**创建**：动态构造沿用泛型构造的既有形式——裸模板名对声明目标推断实例化：`Array<int> a = Array(dims);`（`dims`为`int[]`，秩=`dims.length()`运行期校验，见generics 4.12.3的`Box<int> b = Box(7)`先例）；静态拼写`T[d1, d2]`保留。

**foreach**：既有foreach走线性索引（rank-1与rank-N同址，`VmExecutorOpsArrayN.cpp:153`注释），`Array<T>`上沿用——按elemCount平坦遍历、元素类型T；视图记录经动态op把线性索引换算坐标（除模dims）再按strides取址。

### 4.4 广播

- 规则：numpy右对齐——逐维比较，相等或其一为1则相容，结果维取最大；不相容抛可捕获异常（消息带两个形状，沿用tensor既有消息风格）
- 覆盖：逐元素族（add/sub/mul/div）的数组-数组与数组-标量两种形态（标量=零维广播，偏置加法的基础）；`broadcastTo(Array<E>, int[] shape)`显式视图接口（零步长视图，O(1)，**只读**——写穿抛可捕获错误、消息指明物化路径，见4.2）
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
| 写穿零步长视图（store遇广播维） | 正常用户错误 | 可捕获（numpy readonly view同语义；消息指明reshape物化得可写副本） |

### 4.6 tensor包面（机器学习基本面）

rank轴由`Array<T>`承担，元素种类轴因函数无类型参数保留4条；静态秩旧名（reshape1..8等）**删除**（不向下兼容）。每个函数4条原生声明（int/long/float/double）：

- 创建：`full(int n, T v)`与`full(int[] dims, T v)`（重载）、`arange(...)`（既有）；零填充直接用构造`Array(dims)`（语言内建，无需zeros）
- 形状：`reshape(Array<T>, int[] shape)`（视图；shape允许一个`-1`推断维，非连续源物化）、`flatten(Array<T>)`（视图，等价reshape到秩1。**命名对numpy的偏离，手册须标注**：numpy的flatten恒拷贝、ravel才是视图优先，NLang取ravel语义配flatten名字——无历史包袱下flatten更直白，非连续源照常物化，行为可预期）、`transpose(Array<T>)`（视图，轴全反转）与`transpose(Array<T>, int[] axes)`（视图，任意轴置换，axes须为0..rank-1的排列）、`slice(Array<T>, int[] starts, int[] stops, int[] steps)`（函数形式，全轴slice化，端点与步长语义同下标slice）、`broadcastTo(Array<T>, int[] shape)`（视图，只读，见4.4）
- 逐元素：`add/sub/mul/div(Array<T>, Array<T>)`与`add/sub/mul/div(Array<T>, T)`（广播）；`max/min(Array<T>, T)`（元素级截断，relu基础）；`exp/log(Array<T>)`（激活、softmax与交叉熵基础）
- 归约：`sum/max/min(Array<T>)`全张量（既有）＋`sum(Array<T>, int axis)`沿轴（本轮纳入——步长内核同一泛型循环＋结果分配，见§1.1；max/min沿轴登记roadmap）
- 结构：`matmul(Array<T>, Array<T>)`（秩2校验）、`concat(Array<T>, Array<T>, int axis)`（复制）
- 配套stdlib：随机数直接用**既有math面**——`srand(int)`（带种子后序列确定）、`random()`（[0,1)）、`randomi(min,max)`（闭区间整数），`math.n`现成且刻意裸mt19937算术保证跨平台确定。对抗审核裁定2026-11-03：**不另建random.n**——原设计的`seed/nextFloat/nextInt`三函数是既有面的严格子集，属现状未核导致的重复建设
- 组合层以nlang实现示范：`relu(a) = max(a, 0)`、`softmax(a) = exp(a) / sum(exp(a))`、交叉熵`loss = -log(softmax[y])`（纯nlang代码）——体现"高层特性归应用层"

### 4.7 native ABI与外围

- `NativeArrayView`（C ABI）与宿主表升级，ABI版本v9→v10（v3事实修正：strides早在v8就内联进view并按秩上限64加宽、v9已落`newArrayShaped`，spec前版记"v8→v9"基于过时基线）。v10内容：view结构增`offset`字段；`getArrayView`接受视图记录（现只认`RTK_Array`且按行主序现算strides——改为按记录strides/offset填充，拥有记录维持现算）；新增`newArrayView(host, target, rank, dims, strides, offset)`造视图入口（含4.2创建校验）
- 逐元素/归约内核：步长感知循环（含零步长广播维与负步长逆向维，寻址与符号无关）；matmul：连续检查+非连续物化（转置/负步长视图走物化路径）
- 序列化：v3事实核查发现**数组内容序列化本不存在**（对象图流内建对数组字段一律抛错，.ncu只有类型表无值表；**裸数组值更糟**——`INTR_BS_WriteObject`入口把记录`slot[0]`当classIdx，模块类表超过5项时按`classes[5]`的字段面静默序列化垃圾字节、不超过5项时抛宿主级不可捕获错误——`VmExecutorSer.h`代码级核验；语言面现状被编译门拦下：`stream method "writeObject" expects a class argument`（2026-11-04实证），流方法实参门放行数组后即触此路径，**门的数组放行与运行期kind分派须同批落地**）。按不向下兼容、合理性优先裁定（用户2026-11-03）：**本轮补齐数组内容序列化**——`writeObject(Object)`入口按运行期kind分派（类走既有格式；数组走新tag格式：kind tag＋元素种类＋rank＋dims＋按元素种类分派的载荷，标量直写、struct元素走既有字段序列化器且**tag携带struct名**——自描述读取按名铸struct行，镜像既有类序列化的名入流约定；kind门同时封死上述静默腐蚀路径）；**视图按逻辑内容物化序列化**（numpy pickle同语义）；读取侧新增`readArray()`（自描述：流内tag带元素种类（struct含名）＋rank＋dims，恒产出拥有记录，返回值经`as`转到标注类型复用秩/种类检查）；`readObject(string className)`形态不变（类专用）；对象图中的数组字段不再抛错。jagged本就被拒，元素面封闭可控
- 调试器：数组显示读记录处需认识`RTK_ArrayView`（显示dims/strides/offset，元素沿target解析）
- 陈旧注释修正：`CompiledModule.h:217`"cell 3 + i*cells"改为指向`ArrayHeaderCells(rank)`（v2记的206已漂移；文件在`include/nlang/vm/`）
- 面类解析：静态数组类型的方法查找现走`FindPreludeClassDecl("Array")`（`ExprResolverMemberBuiltins.cpp`）；面类变泛型后，该查找改为对模板行做方法匹配（native方法元数即可，无需T）

## 5 决策理由

- **D1 单一类型模型，方括号为静态秩标注**：v1的两层（独立类型+转换矩阵+物化规则）在对抗审查中三处失守——物化是隐藏O(n)（同一行代码的复杂度取决于不可见历史）；"静态层永不遇视图"靠native契约而非机制；两套类型是永久税而静态层零新功能。单类型下秩承诺改为边界检查：检查价值（坐标数编译错、`rank()`常量折叠、秩立即数指令）全部保留，且无转换矩阵、无物化、无契约。Julia的`Array{T,N}`（秩在类型、边界派发检查）是先例；接口合理性优先、避免过早优化是用户裁定。
- **D2 运行时表示维持RTK_Array，不按class重实现**：前轮已证"class记录装不下变长"的说法错误（`StructSlot`变长记录与数组共存于同一对象堆）。真实代价是两条：元素宽度规则冲突（字段统一8字节步长vs数组按种类4/8）与记录消费者横切特例；收益（声明面+虚派发）已被面类与`Array<T>`真类型拿到。.NET对String的变长尾是同类特例，证明可行而非值得。
- **D3 动态层命名`Array<T>`而非`Tensor<T>`**：与既有`Array`面类连续（零孤儿迁移）、与roadmap"Array升格"一脉相承；`Tensor`名承诺视图语义会造成名字与语义的错位期（tensor包仍叫tensor，类型名不占"张量"一词）。
- **D4 slice用`:`而非`..`**：文法取证——下标内部（`SubscriptExpr`产生式）不含键值冒号，`:`在`a[...]`内完全空闲；键值冒号只活在字面量`{...}`/`[...]`（`nlang.y:2370`起），两上下文不相交。numpy习惯、少一字符；`..`为将来Range类型保留（Rust先例）。用户裁定。
- **D5 视图共享头部前缀**：rank/elemCount/dims与拥有记录同偏移同义，元数据op天然形状无关；只有元素访问分支。深度一（视图恒指拥有记录，组合折叠）使访问与GC不递归，numpy同构。
- **D6 边界秩检查隐式插入**：同一类型无"转换"可言，标注是承诺不是类型；检查失败是用户错误，抛可捕获异常合理（Julia派发失败先例）。`as int[,]`保留为显式拼法（自文档化）。不向下兼容裁定下，`int[,] m = 动态值;`直接可用且可失败——方便优先。
- **D7 广播本轮纳入**：numpy规则是事实标准；标量广播是ML偏置加法的基础；零步长视图机制成本约等于零（记录校验加一条允许零的规则），配套裁定**零步长视图只读**（store一条检查拒写穿，numpy readonly view同语义，见4.2）——零步长别名下写语义本就未定义，只读不是功能损失。matmul保持秩2（批量matmul登记roadmap），KISS。
- **D8 数组进Object**：落实roadmap①，消除`List<Object>.add(int[])`不一致；上转型零分配（区别于字符串装箱）；协议派发沿RTK_Boxed臂既有先例（`VmExecutorOpsObjects.cpp:292-303`）。
- **D9 协议方法身份语义**：与Object默认、Dict身份键、Java/C#数组Equals一致；内容相等留给将来设计。
- **D10 下标同形+运行期坐标数校验**：语言内数组下标不应有两套语法；静态拼写下保持编译期检查与秩立即数指令，动态拼写下把静态op里的"不可能发生的断言"降级为"可能发生的检查"（`VmExecutorOpsArrayN.cpp:56-59`那条比对），无分配无额外间接。
- **D11 tensor秩轴塌缩、元素种类轴保留**：rank轴由`Array<T>`承担；元素种类轴因函数无类型参数而保留4条，是文法约束下的最小形态。
- **D12 jagged继续拒绝**：`Array<Array<int>>`与`int[][]`（两组方括号）是数组的数组，触犯"无jagged布局"不变量（`ExprResolverTypeArgs.cpp:16`）；`List<Array<int>>`与`Array<List<int>>`放行（容器装数组引用已有`List<int[]>`先例）。
- **D13 不做const generics**：`Array<int, 2>`需要值泛型，NLang类型实参只收类型（文法）；Rust实证——`N+1`十五年后仍未稳定（2026年10月官方博客明言不准备稳定），此路成本与收益严重不成比例。Julia的`Array{T,N}`因此不可照抄，静态秩标注+边界检查承担同等价值。
- **D14 批量matmul、slice下标赋值登记roadmap；负标量下标已裁定支持、实现靠后（用户2026-11-01）；沿轴`sum(axis)`移入本轮（对抗审核2026-11-03）**：批量matmul要前导维广播语义、slice下标赋值要视图左值机制，递延理由独立成立；沿轴归约原以「任意轴kernel族」为由递延，本轮步长感知内核建成后该论据失效（同一泛型循环＋结果分配），`sum(axis)`移入本轮、max/min沿轴仍roadmap——独立理由：本轮无消费者（MLP梯度聚合只需sum），`ReduceAxisKernel`泛型模板随sum落地后按需开启是一小步，不预建无用面（KISS，v3.2补记）；负标量下标（`a[-1]`=`len+neg`换算）已裁定支持，因牵连List/string下标一致性，实现顺序靠后，随roadmap登记。
- **D15 验收数据集用公开数据打包子集（用户裁定）**：合成数据可验证性不足（损失下降对破损实现也可能成立，无法对拍）。取MNIST——多层感知机是MNIST的主流体基线模型（CIFAR的主流基线是卷积网络，用MLP跑CIFAR反而不主流），且体量小、3分钟时限余量大。打包子集+官方提取脚本+校验和，兼顾离线测试与可复现验证；同法可换CIFAR（仅基线数值不同）。程序命名`simple_mlp.n`——将来可能有多个MLP相关程序。
- **D16 便利优先裁定（用户2026-11-01：不向下兼容、低成本前提下程序员便利优先）**：六项由roadmap提前——①slice与标量下标混用（降轴=offset折叠+轴剔除，视图构造零新机制）；②负步长slice（stride带符号是视图布局天然能力，逐元素与foreach寻址与符号无关）；③负端点（len+neg一条换算，slice是新语法不触碰既有标量下标规则）；④transpose轴置换参数（dims/strides重排，O(rank)）；⑤`full(int[] dims, T v)`重载；⑥reshape`-1`推断维（一次除法的量级）。仍延后者有真成本，见D14。

## 6 实施步骤

### Step 1 类型层（无视图）

1. `stdlib/lang.n`：`Array`面类升格为`class Array<T>`，增补`equals`/`getHashCode`原生声明
2. 编译器：`Array<int>`放行、方括号=静态秩标注（类型同一、标注随节点）、边界秩检查op插入（赋值/参数/返回）、`as int[,]`显式拼法、null通过
3. 运行时：动态load/store op一对（拥有记录路径，行主序）；`ReceiverClassIndex`数组臂；backing行注册（照List/Dict）
4. 修`OpCheckCast`null瑕疵（4.1节）
5. tensor：32→4声明塌缩（reshape仍复制，Step 2换视图）；随机数用既有math面（见§4.6，不建random.n）；relu/softmax/交叉熵的nlang示范
6. 文档同步（见第7节）、测试（真实编译执行）

### Step 2 视图、slice与广播

1. `RTK_ArrayView`kind与视图记录（共享头部前缀+尾部strides/offset/target、深度一）、视图创建native（`NativeHost`扩展）、创建校验（含零步长）
2. 下标op双形状处理（静态N系与动态系都加视图分支）、foreach视图路径
3. GC视图边（target追踪）
4. ABI v10（view结构offset字段、getArrayView视图臂、newArrayView造视图入口）、逐元素/归约内核步长化（含广播维与负步长）、matmul连续检查
5. reshape/transpose/slice改视图实现；slice语法（下标内`start:stop`与`start:stop:step`产生式、词法零改动）
6. 广播：右对齐规则校验、标量形态、`broadcastTo`
7. 序列化补齐+视图物化（数组内容序列化本不存在，本轮新建：writeObject按kind分派+readArray自描述读取+对象图数组字段；视图按逻辑内容物化）、调试器认识视图
8. 测试（含别名互写可见、GC压测视图挂住target、广播形状矩阵、序列化往返）

### Step 3 验收与收尾

1. **验收：`examples/simple_mlp.n`训练程序**（第8节：MNIST打包子集+`tools/`提取脚本+校验和）作为端到端测试并入测试集，准确率基线实测后冻结
2. roadmap更新：远期特性改写为单一类型模型表述；登记D14的roadmap项（负标量下标标注"已裁定支持"；max/min沿轴归约；批量matmul；slice下标赋值）；进行中0.12.7条目内「新增random.n」「沿轴归约登记远期」两处陈旧表述同步更正
3. 手册规格页（en+zh）终校、VERSION提升、CHANGELOG记录

## 7 影响面与文档同步清单

- 运行时/编译器：`CompiledModuleRecords.h`、`CompiledModule.h`（视图头常量+陈旧注释）、`VmExecutorAlloc.cpp`、`VmExecutorOpsArrayN.cpp`、`VmExecutorGC.cpp`、`VmExecutorOpsObjects.cpp`、array→Object上转型发射点（`src/compiler/CastInfo.cpp`——array-token臂:120起与Object特例臂，v3.2复锚）、`ExprResolverSubscript.cpp`（slice产生式+动态下标）、`ExprResolverTypeArgs.cpp`、`ExprResolverMemberBuiltins.cpp`、`ExprResolverBuiltinClasses.cpp`、`BuiltinNames.h`、`NativeHost.h`、`src/native/tensor/*`、`src/compiler/grammar/nlang.y`（slice产生式）
- 标准库：`stdlib/lang.n`（`Array`面类升格）、`stdlib/tensor.n`（随机数复用math面，不新增包）
- 文档：`docs/user_manual/en/specification/04-types/{array,generics,object,casts}.md`及对应中文页（`Array<T>`唯一类型、静态秩标注语义、slice、广播、Object路径、视图与别名语义）、`docs/roadmap.md`、`README.md`
- 其他：ABI版本v9→v10、调试器数组显示、序列化、`VERSION`、`CHANGELOG.md`

## 8 验证要点与验收标准

**功能验证**：

- `List<Object>.add(int[3])`可用，`get`后`as int[]`取回
- `Object o = int[2,2];`零分配（GC统计无新增记录）；`o.toString()`按数组格式
- `Array<int> a = int[2,3]; a[0,1]`读写正确；坐标数错抛可捕获`IndexOutOfBoundsException`
- 边界检查：`int[,] m = a;`（a实际秩3）抛可捕获`InvalidCastException`（消息带秩3/2）；标注内部`m[1]`仍是编译错
- slice：`int[,] m; m[1:3, :]`返回视图、`m[i, :]`行视图（标注秩1）、越界端点截断、`a[:]`整轴、`a[::-1]`逆向、`a[:-1]`负端点、`start:stop:step`步长；slice结果与源别名互写可见
- reshape/transpose O(1)：改视图dims/strides即完成；transpose axes置换视图、reshape`-1`推断维、非连续视图reshape物化；经视图写入对源可见
- 广播：`add(Array, 标量)`偏置、右对齐形状矩阵（`(3,4)+(4)`合法、`(3,4)+(3)`非法）、`broadcastTo`零步长；**写穿`broadcastTo`视图抛可捕获异常（消息指明物化路径），`reshape`该视图得可写物化副本**；负步长视图（`[::-1]`）读写双向可用
- 既有静态层语义逐字不变：数组测试全绿（含`int[,]`编译期坐标数检查）
- GC压测：视图存活时target不回收；视图与target同死一并回收；元素种类剪枝不受影响
- 序列化往返：裸数组`writeObject`→`readArray()`值相等（int/float/string/**struct**元素与秩1–3，struct经tag内名往返重建）；对象图中数组字段往返；**视图按逻辑内容物化**——视图序列化、反序列化得拥有记录、内容与视图当时逻辑值一致；ByteStream与FileStream两通道同验；`sum(·, axis)`沿轴归约入验证矩阵（批量MLP梯度聚合路径）

**验收标准（用户裁定：方便支持主流机器学习模型，公开数据集可验证、不用合成数据）**：`examples/simple_mlp.n`——两层多层感知机（784→100→10）、手动反向传播、小批量随机梯度下降，在**MNIST官方数据集的打包子集**上训练与测试。数据落地形态：`tools/`提取脚本（官方下载地址、解析、校验和记录在案）+`examples/data/`快照，任何人可复现提取并比对。断言两项：训练损失随轮次下降；测试子集准确率≥基线（基线在程序落地时实测后冻结，写入程序注释；文献锚点按子集规模校准——LeCun等1998年对比表两层MLP 784-300-10误差4.7%是**全量6万训练样本**的数字，子集受限于3分钟测试时限预计只有数千训练样本，该规模下两层MLP常态落在88–92%区间，冻结值显著低于对应规模区间应回查实现而非调低标准；全量数字留作未来放宽时限时的上界参考）。程序必须只依赖tensor包与语言特性，覆盖第4.6节全表面：动态秩批量张量、matmul、偏置广播、relu（`max(a, 0)`）、softmax、交叉熵（`exp/log`）、sum归约、transpose、reshape、随机初始化（计算面限定；数据加载是唯一例外——经fs包按需补齐的二进制读取内建：`readBytes(string path) → int[]`与`readFloats(string path) → float[]`（float32小端整读），MLP数据体量下文本解析不可行（体积与精度双输），归一化等预处理全部前移到提取脚本一次完成、程序零转换直读；用户裁定2026-11-03：可按需补齐必要内建函数/类，实用性与合理性优先）。前向反向取**小批量矩阵形态**（`matmul`＋偏置广播＋`sum(·, axis)`梯度聚合——沿轴最小面已移入本轮，验收程序按MLP最自然的批量写法，不再为规避缺失特性退化为逐样本循环）；softmax按减最大值稳定化。该程序同时作为端到端测试常驻测试集；子集大小与训练轮次调优至运行时限3分钟以内（测试超时约束）。
