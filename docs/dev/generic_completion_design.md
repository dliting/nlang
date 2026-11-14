# 0.13.1a 泛型完善——概要设计

- **文档定位**：0.13.1a的概要设计（Spec）。内容=roadmap条目"0.13.1a：泛型完善"
  （静态引用T全面化+游离函数泛型支持，实现帧隐藏描述符槽与调用点隐藏实参）。
  本文是v0.13.1概要设计（docs/dev/generic_runtime_typeinfo_design.md）D1预留路径
  的兑现，模型权威参考为docs/dev/generics_design.md（2026-10-29用户裁定：存放位置
  必须跟上语言层——static成员引用T与游离泛型函数都要支持）。不向下兼容，合理性
  优先。
- **取证基准**：master树master@a97faa17（v0.13.1发布态），代码位置由全库勘察
  钉死（2026-11-10）。

## 一、现状（取证2026-11-10）

- v0.13.1交付：实例化描述符随对象头部携带（`m_instDescByHeap`并行数组），T操作
  面（typeName/判等/拼接/打印/`as T`/体内构造拼接记录）在实例方法与构造器体内经
  `ReceiverTypeArgOf`读接收者描述符实现；拼接记录唯一帧源是执行帧slot 0（=this）。
- 静态上下文四处具名拒绝（v0.13.1裁决"静态上下文一律拒绝"）：
  DescriptorSpelling.cpp:78-116（T实参构造，"requires an instance context."）、
  ExprResolverTypeGates.cpp:166-206（T操作数运算）、ExprResolverValues.cpp:245-283
  （`as T`）、ExprResolverValues.cpp:434-479（typeName(T)）。
- 跨类静态调用不可达（三链钉死）：`WrapGenericElemArgs`仅在NK_ClassDecl实例化
  上下文内改写实参（ExprResolverGenerics.cpp:335-339）→占位符形参对具体实参无
  CastInfo行→`FixupParamTypesWithBindings`报"Incompatible type"。类内裸调用
  可达（静态命名空间过滤+直调发射已存在）。
- `S<int>.f(5)`是语法错：泛型角形式仅存在于Type/HeadType非终结符（扫描旗标
  OT_TypeLt与plain'<'双拼写家族），表达式位无对应产生式。
- 游离泛型函数不存在：顶层函数记录（CompiledFunction）无模板旗标与类型参数名
  （模板旗标仅CompiledClass有，v0.13.1 D7）；`BindTypeParamPlaceholderIfAny`
  仅在NK_ClassDecl父链绑定T——模块级函数体无T词汇。
- 调用机器：七个调用操作码（OP_CallFunc/OP_CallFuncOut/OP_CallMethod/
  OP_CallMethodDirect/OP_CallMethodDirectOut/OP_CallIntrinsic/OP_CallDelegate）
  汇聚到单一过桥点PushCalleeFrame/PushBoundCalleeFrame（形参拷贝唯一咽喉
  StageFrameParams）；descIdx隐藏u16操作数先例已在五个操作码上落地（OP_New等，
  kStrideThreeWords原位迁移）；按名臂在桥内已读接收者句柄（类行分派用）。
- 帧解剖（VmBackend.h:1879）：`[形参][returnSlot][temp槽×4][callParamBase]
  [evalArea][用户局部]`；CallFrame无任何按帧宿主指针元数据字段。

## 二、方案对照

- **Go**：字典经调用点传递（独立于用户实参的上下文通道），字典内部递归引用
  其他字典（generics-implementation-dictionaries-gcshape.md先例）——对应本文
  D2的0xFFFE转发源与拼接记录上下文源。
- **C#/.NET**：泛型方法（含静态）类型实参从实参推断，推断失败须显式拼写；
  `Box<int>.Create(v)`形态的"实例化类型限定成员访问"合法——对应本文D5双臂
  （推断臂+显式限定拼写臂）。
- **Swift**：泛型函数运行时收元数据（TypeMetadata），调用约定随callee泛型形
  态携带——帧级元数据的先例。
- **承接既有裁定**：generics_design.md已裁定帧槽布局（`__this|形参…|描述符槽|
  帧带区`）、按名/直调双臂填充纪律、游离函数声明与推断规则。该布局草图基于
  旧帧模型；现行帧解剖下本文D1按同等语义重述存放位置形态，语义不变。

## 三、关键设计决策

### D1 帧描述符存放位置：CallFrame字段（原"槽"的现行等价物）

- **决定**：CallFrame新增`const GenericInstDescriptor* instDesc`字段（默认
  nullptr）。CompiledFunction新增`isGeneric`线旗标；旗标为假的函数帧零改动
  （字段保持null，不做填充）。
- **为什么不是localsBuf槽位**：原模型草图"形参区之后的首个固定槽"基于旧帧
  布局。现行帧解剖中插槽会分叉泛型函数的全部偏移计算（returnSlot/temp槽/
  callParamBase/evalAreaBase/帧带起点/outShift回写算术），而语义需要的只是
  "按帧可读的宿主指针"——CallFrame字段与resultCell/outMask同族，帧尺寸、
  形参拷贝长度、帧带分类、GC扫描（字段指向进程级驻留池，宿主内存，永不扫描）
  全部不受影响。
- **帧源语义统一**："当前实例化上下文"=执行帧的instDesc。实例方法帧的上下文
  由接收者头填充（与v0.13.1对象存放位置值恒等）；静态成员帧与游离函数帧的上下文
  由调用点填充（D2）。v0.13.1的`ReceiverTypeArgOf`（slot 0读）至此退役为
  帧字段读。

### D2 描述符填充：单一过桥点，三源裁决

- PushCalleeFrame/PushBoundCalleeFrame（唯一过桥点）按callee->isGeneric填充
  instDesc，源按调用形态：
  1. **直调臂无接收者**（静态成员、游离函数）：调用操作码携带u16 descIdx
     （0xFFFF=无，0xFFFE=转发调用者自身instDesc）。descIdx指向模块描述符表：
     静态拼写行run-start已驻留；拼接记录行在过桥点按调用者上下文即时合成（复用
     CastTargetDescriptor(descIdx, callerLocals)，"当前上下文实参"语义）。
  2. **带接收者**（实例方法直调与按名虚分派，含OP_New内部调用的构造器帧与
     super()基类构造）：桥内已有接收者句柄（类行分派读取），补一行
     InstDescriptorOf填入。接收者无描述符（擦除行对象）→槽为null，T操作
     沿用v0.13.1的降级语义（运行时具名错误）。
  3. **委托**：绑定委托（有接收者）随头描述符填充；对泛型游离函数/泛型静态
     成员取函数值（OP_MakeFunc族）具名拒绝——无描述符的函数值是诚实模型，
     描述符配对存储随v0.13.10 lambda版本演进。
- **操作码挂载面裁决**：无接收者直调从今天起独立成操作码——OP_CallFunc/
  OP_CallFuncOut槽位更名为OP_CallStatic/OP_CallStaticOut，操作数追加u16
  descIdx；静态成员调用与游离函数调用统一路由至此（两者同为无接收者直调，
  语义诚实分离，`EmitMemberStaticCall`改走游离调用助手）。OP_CallMethodDirect
  (Out)（实例直调热路径）与按名族、intrinsic不带操作数，字节零增长。
- native/intrinsic臂不经VM帧：泛型native成员走ABI v12接收者定位（v0.13.1既有），
  零改动。
- stride组随操作数增长原位迁移（OP_New先例），枚举顺序不动（槽位复用）。
- 非泛型callee：一次旗标测试，零填充（descIdx=0xFFFF直过）。实例调用路径无
  新开销；泛型帧填充=两次指针读。

### D3 T操作面统一读上下文

- 五个T操作读位置（OpTypeNameArg/OpTypeParamEquality/OpTypeArgToStr/
  OpAsTypeParam/拼接记录tag 2臂）从slot 0接收者读改为执行帧instDesc读（语义改名
  "当前上下文实参"；线格式拼接记录tag字节不动，全量重编）。
- 四处编译期静态拒绝门改键：从"所在函数isStatic"改为"所在函数是否泛型记录"
  （泛型类的静态成员与游离泛型函数放行，其余语境不变）。静态字段解禁仍非目标
  （补偿模式仍是静态字段的官方答案；静态**方法**不再需要补偿）。
- 语义不变式：实例方法体内一切T操作行为与v0.13.1逐位一致（上下文=接收者描述
  符）；共享方法体单一代码生成路径（D1帧源统一的结果）。

### D4 游离泛型函数

- **声明语法**：类型参数表写在函数名后（声明位无表达式歧义，既有裁定）：
  `T max<T>(T a, T b)`、`K V dictInvert<K, V>(Dict<K, V> src)`。顶层函数全部
  声明形态（TopBareHeader/TopModHeader族）加泛型臂。
- **函数级T绑定臂**：`BindTypeParamPlaceholderIfAny`增加NK_FunctionDecl父链臂
  （类臂对齐：函数记录携带typeParamNames，按名绑定占位符）。
- **记录模型**：一份函数记录（isGeneric+typeParamNames），体一次编译，签名
  参数化保留占位符叶——与类模板行同构；按名注册（无重载，维持现状）。
- **调用点类型实参推断**（新机制，C#先例）：合一实参静态类型对形参类型树——
  T直接出现、`T[]`（秩一致）、嵌套实例化逐层（`List<T>`对`List<int>`）；仅返回
  位出现时从赋值目标推断。合一为精确匹配，不做隐式数值提升（`max(3, 7.0)`
  报不一致，Go/C#同例；用户显式写`max(3.0, 7.0)`）。全部形参位与返回位合一
  完成后：不一致→具名诊断；推断不出（T不出现在任何可推断位）→具名诊断。
  实参种类约束沿用0.13.0/0.13.1现状（struct实参合法；func类型实参拒绝）。
  T形参默认值拒绝（与类模板T字段初值同线：无约束下不存在有意义的T初值）。
- **签名边界替换，无克隆**：函数调用只需在签名边界做类型检查与返回类型替换
  （合一器直接对占位符类型树行走，复用CastInfo判定原语），不需要类模板的两阶段
  AST创建——被调记录始终是模板记录（FunctionSlotFor经RecordOwnerOfMember解析
  克隆→模板的通道本就存在，泛型游离函数无克隆）。递归/泛型调泛型：0xFFFE转发
  或拼接记录合成（T流入实参时）。
- **跨包分发**：`.ncu`函数记录携带isGeneric+typeParamNames（D6）；导入桩重建
  泛型函数声明（`SynthTypeExprFromDesc`的RTK_TypeParam臂合成签名叶，函数级绑定
  臂绑占位符）；使用方调用点推断对桩签名工作，直接调用模板记录（字节码随模块
  链接分发，免重编译——Rust元数据分发先例，既有裁定）。

### D5 静态成员调用面

- **推断臂（主路径）**：泛型类的静态成员经类名限定调用`S.f(args)`——类型名
  接收者→静态命名空间（过滤已存在）→被调形参含占位符叶时进入推断模式（通用化
  WrapGenericElemArgs的NK_ClassDecl上下文门：推断上下文=被调模板行+实参，不再
  要求类实例化语境）；推断产出类型实参→静态拼写描述符行（编译期全具体）→
  descIdx直调。
- **显式限定拼写臂**：`S<int>.f(args)`——表达式位泛型拼写产生式（OT_TypeLt
  双拼写家族，HeadType语句头先例）；`S<int>`经既有类型位解析臂
  （TryResolveDeclaredGenericInstance对齐）在表达式位产出实例化类型接收者，
  静态命名空间分派照走。实施时必须以冲突计数实验证明与比较链
  （`a < b > c`形态）无新冲突，对齐既定冲突基线纪律。不引入方法名后的显式
  实参语法（`max<int>(...)`，既有裁定维持）。
- **推断失败**：具名诊断点名形参并提示限定拼写形态（"cannot infer type
  argument for T in S.make(...); spell the instantiation: S<int>.make(...)"）。
- 类内裸调用（现状可达）：泛型调用者0xFFFE转发；非泛型类静态调用零变化。
- 全限定类型接收者（lib.Util.f）维持文档化限制（本版不动）。

### D6 `.ncu` v2.17

- CompiledFunction线格式追加：isGeneric旗标、typeParamNames（CompiledClass
  v0.13.1字段的函数侧镜像）；T形参paramTypeDescs携带RTK_TypeParam叶（占位符叶
  既有线格式，桩重建降级路径同步函数化）。
- 调用操作码操作数增长（D2）；描述符表拼接记录行跨单元合并既有（v0.13.1 Task 3）。
- NCU_FORMAT_MINOR 16→17，loader拒绝≤16（记录布局与操作数布局位移；全部
  .ncu随版本重编，既有惯例）。

### D7 宿主ABI不动

- ABI维持v12：native路径不经VM帧，泛型native成员的接收者定位（isInstanceMethod
  旗标）v0.13.1已交付。泛型native游离函数非目标（native注册按名单记录，需ABI
  演进，无消费者拉力）。

### D8 版本与文档

- VERSION→0.13.1a：版本管线容忍字母尾（CMake project()取数字前缀，CPack保留
  全串→制品名NLang-0.13.1a-win64；VERSION正则放宽为可选小写字母尾）。
- CHANGELOG中英成对；语言规格：泛型章（游离泛型函数+调用点推断）、静态成员章
  （静态引用T放行与调用形态）、运算符页（T操作面在泛型函数语境的语义）、异常页
  不变；用户手册同步（补偿模式章节改写：静态字段仍需补偿，静态方法直接可用）；
  `tools/manual_sync_check.py`核销漂移；README随接口变更同步。

## 四、非目标

- 方法级类型参数、类型参数约束、模板特化、泛型接口/结构体（既有裁定维持）。
- 方法名位显式实参语法（`max<int>(...)`）；表达式位`S<int>`除静态调用接收者外
  的其他位置（赋值目标/实参位仍拒绝——类型位文法外溢需独立裁决）。
- 泛型函数的函数值/委托引用（MakeFunc具名拒绝，配对存储随v0.13.10 lambda）。
- 泛型native游离函数（D7）；泛型类静态字段解禁（补偿模式仍为官方答案）。
- 表达式位`typeName(expr)`值形态（v0.13.1非目标维持）。

## 五、实施顺序与验收

- 顺序（依赖排序，机制先行）：
  1. 帧存放位置：CompiledFunction.isGeneric+CallFrame.instDesc字段+过桥点填充三源
     （按名臂接收者头先落，直调操作数随后）。
  2. T操作面换读（D3五位置+拒绝门改键）；实例语义回归锁定（v0.13.1全量e2e
     不翻）。
  3. OP_CallStatic(Out)更名与descIdx操作数（stride迁移+静态调用路由统一+
     反汇编/ndb/链接器步进lockstep）+0xFFFE转发。
  4. 游离泛型函数声明文法+函数级T绑定+记录注册（isGeneric+typeParamNames）。
  5. 调用点推断器（合一行走+具名诊断+返回位推断）接EmitFreeFunctionCall。
  6. 静态成员调用面（推断臂通用化+`S<int>.f`表达式位文法）。
  7. `.ncu` v2.17序列化+桩重建+跨包调用往返。
  8. 版本管线（0.13.1a）+文档/手册/CHANGELOG收尾。
- e2e钉：`S.f(args)`跨类推断全种类族（标量/string/struct/类/枚举/数组/嵌套
  实例化）；`S<int>.f(args)`限定拼写；推断失败与"推断不出"具名诊断（含
  `max(3, 7.0)`不一致）；类内裸调用转发；静态体内typeName/判等/拼接/打印/
  `as T`全操作面；泛型静态/游离体内以T实参构造（`Hold<T>`拼接记录臂）；游离泛型
  函数声明+调用+推断三形态（T直接/T[]/嵌套）；仅返回位推断；泛型调泛型（转发
  与拼接记录两臂，含泛型类静态调游离函数）；递归；跨包无源码包泛型函数调用往返；
  MakeFunc泛型拒绝诊断；擦除行对象降级路径（T操作运行时具名错误）；泛型静态/
  游离帧T形参GC回归族（gc-stress）；v0.13.1全量实例语义e2e回归不翻；ndb步进
  OP_CallStatic布局。
- 门禁：Debug断言＋ASan＋IDE树全量；e2e串行；语言规格与手册同步为完成定义。
