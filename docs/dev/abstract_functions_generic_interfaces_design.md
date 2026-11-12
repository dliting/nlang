#0.13.1c概要设计：抽象函数、泛型接口与遗留语法收口

##1 背景与范围

roadmap 0.13.1c条目五项：

1. 类级抽象函数（纯虚函数）与泛型接口。
2. 拼写歧义具名指向探针（identity规则，以RejectTypeNameVariable名字分区为健全性基础；先设计后编码）。
3. 数值基本类型的宽度别名拼写（int8/int16/int32/int64、uint8/uint16/uint32/uint64、fp32/fp64全族）。
4. 移除试验遗留保留字`elseif`与`state`。
5. 类型参数数组创建拒绝门的裁决与收口。

本文是概要设计（spec），给出各项的语义裁定、机制轮廓、站点清单与非目标；实施计划另行成文。探查代理盘点与探针实测的关键事实随文记录，取证出处见§10。

##2 抽象函数（纯虚函数）

###2.1 现状事实

- 语言没有`abstract`关键字；`NF_Abstract`是解析器内部位，凡无体函数形式（`;`收尾）都由文法动作置位（nlang.y约1900-1985），但类方法带此位时：解析器静默（StatementResolverDecls.cpp:62 `if (!sn.Body()) return`）、后端不注册记录（Register.cpp:163-166）、被调用才在emit期报"own function missing from the unit table"（Slots.cpp:39）。无体方法今日是"编译通过但不可用"的空洞。
- 接口实例方法强制无体并置NF_Abstract（nlang.y:1826-1842），有体实例方法是具名诊断（StatementResolverTypes.cpp:447-449）。
- 虚分派无vtable，按方法名派发：`OP_CallMethod`+名字索引→`ResolveVirtualCallee`→`FindMethodByName`沿超类链线性扫（VmExecutorOpsCalls.cpp:269-368）。有`virtual`标记才走名字派发，否则直调函数槽。
- 覆盖契约设施齐备：签名同一性`SignaturesIdentical`（ModuleBuilderMethodContracts.cpp:57-81）、契约集=祖先同名虚函数+接口同名方法（CollectContractSet:136-170）、接口义务完备性`CheckInterfaceContracts`:411-456（沿链收集、首个同名分派胜者必须签名同一）。
- 跨模块：无体方法不进线格式，导入侧重建桩时看不到它。

###2.2 语义裁定

- **拼写**：类中无体方法形式`ret name(params);`即抽象声明，与接口成员同形，无新关键字。`;`本身就是纯虚标记（对照C++的`= 0`后缀，learncpp 25-7）。豁免：文法动作对带NF_Native的无体形式不加NF_Abstract——native声明全部无体且无`abstract`拼写，不豁免则每个native方法都被误判为抽象（现行文法对所有无体形式一律置位，须改）。
- **抽象蕴含虚**：无体方法的派发只能按名字进行，故抽象方法自动获得NF_Virtual，用户写不写`virtual`语义相同。主流佐证：C#/Java的abstract方法隐式虚；C++要求显式`virtual`但我们不采纳，因为NLang接口成员本就无virtual拼写而按名字派发，抽象方法与接口成员同形同义更一致。
- **隐式抽象类**（C++先例）：类自身或沿继承链存在未实现的抽象方法即抽象类，`new`具名拒绝；子类实现全部抽象方法（签名同一）才具体，否则仍抽象。沿链抽象性算法以分派胜者规则钉死：对每个抽象义务沿链取同名分派胜者（FindDispatchWinner同款行走，镜像运行时FindMethodByName），胜者仍是抽象声明则义务未满足——这保证编译期抽象性判定与运行时分派永不背离（C#式再抽象语义：再抽象屏蔽祖先具体实现）。不引入类级`abstract`修饰符——成员形态已携带全部信息，加修饰符是冗余状态（两处事实源会漂移）。
- **义务检查（四分规则）**：祖先抽象方法并入契约集后，未覆盖项按义务来源分派，不能一律报错（既有ReportUnmatchedContractSignatures是硬错误，直接沿用会使一切未实现抽象方法的子类编译失败，抽象类将无法声明）：
  1. 未覆盖的**抽象祖先**义务→不是错误，计入抽象性计算，进`new`拒绝门的消息；
  2. 同名方法签名不一致→报错（own侧不匹配报错不变：CheckOverrideSignatures的MatchOwnMethodsToContract；其未覆盖报告臂ReportUnmatchedContractSignatures按义务来源分派——这正是本版改动点）；
  3. **接口**义务未实现→报错（维持现状）；
  4. `new`拒绝消息列出未实现的抽象方法名（首个即可，按声明序）。
  子类允许把继承的抽象方法再声明为抽象（义务继续下传），此时子类仍为抽象类。
  交互注记：FindDispatchWinner+SignaturesIdentical（ModuleBuilderMethodContracts.cpp:430-441）不查体——继承的抽象声明按签名"满足"接口义务是合法状态，类保持抽象由沿链抽象性规则兜住；实施时不得给分派胜者加"必须有体"检查。
- **实例化拒绝门**：`new`解析处（ExprResolverNew.cpp SelectCtorForArgs路径）查类是否抽象，拒绝消息列出未实现的抽象方法名（首个即可，按声明序）。抽象类的构造器保留：具体子类经`super()`调用（既有机制，StatementResolverSuper.cpp/EmitStmtTry.cpp:208-270）。
- **抽象类类型变量合法**：持有具体子类实例，上溯造型既有（CastInfo.cpp:264-281同指针无运行时开销）。
- **冲突诊断**（只限真正可拼写的形态）：静态无体方法（顺带收掉今天的无体静态空洞）、无体构造器，各出具名冲突诊断。native+抽象不可拼写（见拼写豁免）。
- **排除面**：struct无虚分派，抽象方法拒绝；枚举方法已有"must have a body"（StatementResolverDeclEnum.cpp:29-34）不动；泛型类中抽象方法放行（既有"must be native or have a method body"规则放宽为"must be native, abstract, or have a method body"），实例化铸造照抄模板义务；模板抽象则实例化抽象，`Box<int>()`这类实例化使用位同样被实例化拒绝门拦下（抽象性随模板传导）。
- **函数句柄绑定**：抽象方法可被绑定为Func句柄（ExprResolverFuncRef路径），调用时运行时报错——与C#/Java一致的有意选择（放行绑定、运行时拒绝），实施计划须为此路径加测试钉子。
- **入口**：main无体已有拒绝（Register.cpp:371-380），不动。

###2.3 线格式与运行时

- 无体方法今日不进线格式，跨模块抽象义务会静默消失——必须入线。`CompiledFunction`加isAbstract位：抽象方法注册签名记录（无码），镜像isNative的做法；`CompiledClass`加isAbstract位——编译期沿整链算好显式入线（派生类未实现祖先抽象方法时自身并无抽象方法记录，只从自身方法表推导会漏，必须编译期定值显式携带；CompiledClass已有isConstAggregate单线字节与isTemplate先例，镜像可行）。加载侧桩重建把NF_Abstract带回AST，跨模块子类义务检查照常工作。
- 线格式两表（类、接口，见§3.5）都动，本版ABI一并升版。
- 运行时防御：`FindMethodByName`命中isAbstract记录时报"abstract method has no implementation"——抽象类不可实例化使其实际不可达，此防御只是兜底。

##3 泛型接口

###3.1 现状事实

- 接口文法无基表产生式（nlang.y:1810-1815），接口间继承不支持（SnMisc.h:448-450注释、ExprResolverCast.cpp:184）——本版仍不支持，见非目标。
- 泛型解析的类门：`FindDeclaredGenericTemplate`要求`NK_ClassDecl && IsGenericDeclaration()`（ExprResolverGenericUse.cpp:110-128）；铸造`GetDeclaredGenericClassDecl`产出SnClassDecl壳（ExprResolverGenericMint.cpp:241-272）；注册表键与实例槽都是SnClassDecl*形。
- 接口线格式`CompiledInterface`无isTemplate/typeParamNames（CompiledModuleRecords.h:365-375）；TypeDesc对接口类型无RTK，降级RTK_NonSerialized（TypeDesc.cpp:296-299）。
- 类型实参面已就绪：`RejectMultiSlotTypeArgs`明确允许接口值作实参（"an interface value is a single-slot object handle"，ExprResolverGenericUse.cpp:64-65）。
- 接口调用按名字派发（EmitExprMemberCall.cpp:126-168），泛型类方法的边界装箱计划（EmitGenericBoxing.cpp CollectTemplateMethods/PlanGenericMethodBoxing）只读SnClassDecl::Members。

###3.2 语法与解析

- InterfaceDecl加TypeParamsOpt（与ClassDecl头同产生式族），声明位无表达式歧义（既有裁定）。
- 泛型接口的使用位解析：把FindDeclaredGenericTemplate的类门放宽为类或接口；铸造路径做接口对应物——SnInterfaceDecl壳+成员克隆+占位符替换（SubstituteTypeExpr机制共享）；arity拒绝镜像类的消息族。注册表键需要接口形（模板指针统一为SyntaxNode或加SnInterfaceDecl*通道，实施计划细化）。
- implements子句带类型实参：现NameList只收裸名（nlang.y:1793-1800），扩展为可带`<...>`实参的名字项；`class C implements I<int>`合规性检查对实例化后的签名做（与类泛型同法）。

###3.3 成员契约

- 泛型接口方法签名可含T（参数/返回），语义与泛型类方法一致：T值跨界按实例化kind处理（通道拆分见§3.4）。
- 接口常量的类型不得是类型参数：常量槽的kind在实例化前未知，与泛型类"T类型字段初始化器禁止"（GenericDeclContracts.cpp:67-79）同族，出同风格具名诊断。
- 泛型接口方法的T类型形参默认值沿用泛型类同名拒绝（GenericDeclContracts.cpp:158-168消息族）：接口签名入线格式承载defaultValues（CompiledInterfaceMethod），不设门则实例化后默认值错配。
- 其余成员契约（public、静态有体、实例无体、禁native禁virtual）照旧。

###3.4 分派与装箱

- 接口**实例**方法按名字派发，描述符经接收者对象头填充（OpCallMethod的thisHeapIdx通道）——不需要调用点隐藏实参，实施时不得给实例调用接入隐藏实参通道。
- 泛型接口的**静态**成员走直调，调用点装箱沿EmitGenericBoxing既有计划机制，CollectTemplateMethods（现只读SnClassDecl::Members）加接口成员读取通道。
- 装箱计划的占位符序号、255上限、0xFF无序号哨兵全沿既有（0.13.1b已定），无新机制。

###3.5 线格式

- `CompiledInterface`加isTemplate+typeParamNames；签名表的paramTypeDescs本就承载占位符描述符recipe（描述符表机制共享），接口类型实参的跨模块桩重建可用。
- TypeDesc对接口类型的RTK降级不在本版处理（该降级影响的是"类字段类型为接口"的序列化面，与本版功能无交；记入递延清单）。

##4 拼写歧义identity规则探针

###4.1 现状与缺口

- 形状判定有两层：词法器对内置泛型名置PendingGenericOpen旗标，0.13.1a又加形状探针ProbeTypeArgumentOpener（ScriptScanner.cpp:199-260，标识符后越过空白/注释见`<`，前向判定`<...>`是否形状良构的类型实参表，外字符`" ' ( ) { } ;`中止，4096字符界）；声明的泛型名不走词法武装，其使用位由文法的plain-'<'形状产生式族承担。
- 形状规则的碰撞实证（0.13.1b提交8292b26a）：`both(a < b, c > (d))`被文法形状产生式读成`a<b, c>(d)`调用。0.13.1b交付了三具名事后诊断（值头部给出加括号比较形式、函数头部、非泛型类型头部）。
- 递延缺口：事后诊断只指路不放行——正确程序（实参本是比较）仍编译失败，须用户手动加括号。

###4.2 健全性基础

RejectTypeNameVariable名字分区（StatementResolverTypes.cpp:57-65）：凡在此能拼写为类型的名字，不能在此作变量名；NamesVisibleType是其补谓词。推论：**头部名字若解析为值，则类型读法在身份上不可能**，比较读法是唯一健全读法——这正是identity规则。健全性再进一步：语言允许值种类与类型名碰撞的场合（枚举成员等），救援只在类型探针完全未命中同名泛型模板时触发（解析次序先类型后值，ExprResolverGenericUse.cpp:188-191），同名枚举成员不会误触发。

###4.3 设计：解析期身份救援

- 词法器保持形状武装（它无法查名字表；声明的泛型名要靠它放行`Box<int>()`表达式拼写，弱化会回归既有功能）。
- 解析完成后进入解析器：形状读法成立的泛型实例化拼写，头部名字解析命中值时，不立即报0.13.1b诊断，先做**身份救援**——该类型读法已被身份规则证伪，比较读法唯一健全，于是对被误读表达式子树的源区间重扫描重解析（扫描器保留全文与行首表，支持开串扫描；区间=实例化拼写头部记号到被误读调用收括号，子树各记号位置可取）。比较读法在实参表级重建（`a < b, c > (d)`重读为两个实参`a<b`与`c>(d)`），重建结果替换外层调用的实参表（AST手术走既有Children/DetachChild设施），再走正常解析。
- **新增工作项**：ScriptParser今日只有ParseUnit一个入口，片段重解析需要新增实参表片段解析入口（第二入口或包装单元形态），含片段坐标到绝对位置的偏移交接；片段产生式与单元产生式共享文法时注意LALR冲突面（形状产生式是承重墙，不得扰动）。
- 救援失败分两类处理：片段**语法**解析失败（重扫描不产良构表达式）→回落0.13.1b具名诊断，行为不劣于现状；重建后的比较式**语义**失败（如`a < b`作用于string）是用户真实代码错误→按真实错误原样上报，不得回落到形状诊断掩盖真因。
- 身份救援只覆盖"值头部"形态；函数头部与非泛型类型头部不满足健全性推论的前提（名字解析为函数/非泛型类型时类型读法并未被证伪），维持0.13.1b诊断。救援成功后，用户手动加括号的写法（`f((a < b), (c > d))`）语义不变，双形态并存。
- e2e钉子：`both(a < b, c > (d))`无括号直通（比较读法）、保留的三具名诊断回归、形状仍成立的真实泛型拼写不回归。

###4.4 备选否决记录

- 词法期身份判定：声明的泛型名在解析期后才可见，词法器查不到，否决。
- AST结构重折（不重扫描）：逗号分隔的结构折叠对N个实参形状本可良定义，但嵌套实例化形状（类型实参表内再含`<...>`）覆盖不全且易漏，重扫描以源文本为准无此盲区，否决。

##5 基本类型别名

###5.1 主流参照

- Go：内置宇宙里byte/rune是真类型别名（`type byte = uint8`、`type rune = int32`），别名与本体在类型系统中全同（go.dev/builtin；go101别名节）。
- C#：int与Int32固定互为别名；`using`别名指令可别名到基元类型（MS Learn using directive）。
- 两者都允许别名拼写被内层作用域遮蔽，但见下——NLang的名字分区不变量使遮蔽无意义。

###5.2 裁决：词法关键字别名

NLang名字分区不变量（RejectTypeNameVariable族）裁定：凡能拼写为类型的名字不能作变量名。宽度别名拼写一旦成为类型名，全库任何作用域都不能再声明同名变量——"可遮蔽"没有任何存活空间，别名拼写事实上就是保留字。既然保留是语义必然而非实现选择，最诚实的最小机制是词法关键字：按宽度系统性覆盖全部数值基本类型，每条词法规则把别名拼写映射到本体类型的既有KT_记号——int8→byte、int16→short、int32→int、int64→long、uint8→ubyte、uint16→ushort、uint32→uint、uint64→ulong、fp32→float、fp64→double（宽度对应已按手册§4.2类型表核对钉死；bool/char/string无数值宽度，不设别名）。修订记录（2026-11-12）：初稿以int32/int64/fp32/fp64为例，用户澄清例非穷尽，按宽度系统性扩成全族。

- 下游零改动：AST只见既有NK_*节点、描述符拼写与线格式都是canonical名（`typeName(int32)`折出"int"），别名是纯拼写层。诊断输出同理显示canonical名（写`int32`触发的错误显示`int`），手册别名小节说明之。
- 冲突面已查：stdlib九包、tests/e2e对全族拼写零代码命中（2026-11-12两轮全量grep）。
- 否决"预置包using别名"方案：using别名不跨编译单元（探针p9main2实证），预置合并不带别名符号，方案需先做"别名出单元"的语言改造，超出本版范围且收益为负（见上，保留不可避免）。
- 手册：保留字表加全族拼写（与§6的删除同批），§1.4加基元别名对照表；措辞表驱动不写死数量，将来扩表即扩语言。

##6 移除保留字elseif与state

站点（roadmap已钉）：nlang.l:241/272两条关键字规则、nlang.y:406/429两条记号声明删除，二者成为普通标识符；nlang.l:391-403过时的`List < 3`误读限制注释删除（名字分区规则后场景不可达）；手册en/zh保留字表删两词、§1.4"是否保留与语法使用无关"句删除、§6.3"单词elseif形式是语法错误"按新行为改写；e2e补"二者可作标识符"钉子。源内elseif只在nlang.l一处（全量grep实证），langservice/embed无独立关键字表。

##7 类型参数数组创建拒绝门收口

###7.1 实测矩阵（2026-11-12探针）

| 形态 | 现状 |
|---|---|
| `T slot[3];`（定长声明） | 已拒（RejectTypeParamArrayElement，唯一门，调用点StatementResolverDecls.cpp） |
| `T[] arr = T[2];`（表达式位创建） | 编译通过；元素写对所有绑定（int/string/class）全部拒绝，元素运算拒绝，仅length()可用——构出物不可用 |
| 元素写拒绝消息 | 值按兼容家族分别拒绝，与"元素kind未知"的根因不符 |

三口径互斥：门只拦声明形态、手册en/zh泛型页声称一律拒绝（generics.md:64-67）、设计注释D2把两形态视为同一拒绝家族（GenericDeclContracts.cpp）。

###7.2 裁决：收严

放行需要"按实例化的元素kind类型化数组"，工作量与v0.13.2数组重设计重叠；而实测显示表达式位创建的构出物对一切绑定都不可写不可用——没有任何有用的程序形态，放行没有价值。收严：在`Access(SnNewArrayExpr&)`元素类型解析后（ExprResolverNew.cpp:118-157）加类型参数占位检查，谓词与既有门完全一致（解析后的元素类型字段命中`DeclaredGenerics().IsPlaceholder`，GenericDeclContracts.cpp:89-100同款判定，天然覆盖泛型自由函数体内的`T[2]`），拒绝消息复用既有文本（"Cannot create an array of the type parameter \"%s\"..."）逐字保持一致。`Box<T>[n]`这类"实例化作元素类型"是合法句柄数组，不在门内。

- 门落位后D2注释的"同一拒绝家族"陈述为真，注释不动。
- 手册en/zh泛型页声称的"一律拒绝"成为事实，核对措辞后无需改写（除非措辞与新门行为有出入）。
- e2e钉子：表达式位创建具名拒绝、声明位维持既有钉。

##8 非目标

- 接口间继承（无基表产生式、无运行时；既有裁定递延）。
- 类级`abstract`修饰符拼写（§2.2裁定冗余）。
- using别名跨编译单元导出（§5.2裁定无存活价值）。
- T数组创建的放行路线（§7.2裁决收严）。
- TypeDesc接口RTK补全（§3.5递延；TypeDesc.cpp:296-297处"CompiledModule has no interfaces table"注释已过时——表已存在缺的是RTK种类，随本版顺手改注释）。
- 游离函数无体形态（`void f();`顶层）维持现状（roadmap只收口类级）。

##9 测试与门禁策略

- 手册是完成定义的一部分：抽象函数与泛型接口在语言规格新增对应章节（en/zh成对），别名与保留字改动按§5/§6落表；随实现同批提交，不攒到版本收尾（tools/manual_sync_check.py流程）。
- 每项e2e钉子清单随实施计划给出，钉子先行（TDD）。
- 单元测试覆盖新诊断与契约分支；真编译真执行（编译器编译.n、VM执行字节码）。
- 门禁按例行收尾：Debug（断言）+ASan+IDE树三树并行重建、e2e串行；慢测试只在提交前批次跑；判决只认tools/gate.sh判决行。
- 版本收口：VERSION升为0.13.1c（延续0.13.1b的字母递进惯例）、CHANGELOG记录。

##10 取证出处

- C++纯虚函数使类自动抽象、派生类未实现全部纯虚则仍抽象：learncpp.com 25.7 "Pure virtual functions, abstract base classes, and interface classes"。
- Go内置别名byte=uint8、rune=int32（真别名，类型系统全同）：go.dev/builtin文档；go101.org类型别名章节。
- C#基元using别名合法、int≡Int32固定别名：Microsoft Learn "using directive"（C#语言规范）。
- C#abstract方法隐式虚、Java方法一律虚分派：Microsoft Learn C#参考"abstract"；JLS §8.4.8。
- NLang现状事实：探查代理盘点报告（本文§2.1/§3.1引述）与temp/probes_0131c九枚探针实测（2026-11-12）。
