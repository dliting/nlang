/*---
v0.13.8 跨模块功能完善概要设计（roadmap条5/6/7/8/9）

范围：跨模块可见性访问控制（条5）、跨模块接口与泛型类型签名完整化（条6）、
限定泛型拼写解析（条7）、泛型描述符规范拼写改包限定链接身份（条8）、
类继承泛型实例化作基类（条9）。条1/3/4经2026-11-15核查为陈旧登记，
已于0.10.14交付，不在本设计范围（见roadmap 0.13.8条目修正说明）。

状态：概要设计定稿（详细设计归实施计划）。已完成现状测绘、主流平台取证
与对抗评审循环完善五轮（2026-11-15：首轮2C/4I/3M、二轮1C/1I/1M/2L、
三轮1I、四轮1M/3L、五轮零新发现——各轮发现全部独立核实并回写本稿；
探针实测：`x as y < z`语法错误、`x as y`语义错误即解析通过）。
---*/

# 0. 现状坐标（2026-11-15实测钉死；含评审修正）

- 导入管线：`src/compiler/ModuleBuilderImports.cpp`（LoadImports→LoadExternalModule→RegisterExternalStubs）；桩合成在`src/compiler/builder/CompiledModuleNodeBuilder.hpp`与`CompiledModuleStubMembers.hpp`/`CompiledModuleCtorStub.hpp`；两阶段合并：PrepareUnits（ModuleBuilder.cpp:56-99）阶段序＋MergeTransUnits。
- 线格式：`include/nlang/vm/CompiledModule.h`（NCU 2.19；v2.15模板描述符表已上线——每单元一行的实例化描述符，规范拼写+组合记录）；记录形状`CompiledModuleRecords.h`。导入注册表在线：SymbolImport带modulePath（目标单元点分路径，CompiledModule.h:382-410；functionImports/classImports/structImports/enumImports/staticSlotImports五段）。宿主ABI v12（NativeHost.h:75）。
- 条5现状：自由函数文法已收访问前缀（TopModHeader族，nlang.y:729-775）；**命名空间级成员（自由函数与顶层类型 alike）默认FA_Public**（SyntaxNode.cpp:252-260，SnField::OnAddedToParent的NK_Namespace/NK_InterfaceDecl/NK_StructDecl/NK_EnumDecl臂；类成员与段落级成员默认private）——今天跨模块自由函数调用可用，正是因为默认public且桩硬编public。类型声明无访问前缀位（nlang.y:1701+直接KT_Class）。CompiledFunction无线格式访问字节；导入自由函数桩硬编FA_Public（CompiledModuleNodeBuilder.hpp:134-164），且叶名空闲时桩直入Root()->Members()（:151-163）——非限定名查找可绑到导入桩。类成员访问字节在线上（fieldAccess/methodAccess/constructorAccess），加载器不做检查，检查全在编译期（AllowAccess族）。
- 条6现状：接口臂降级RTK_NonSerialized（TypeDesc.cpp:293-296；"no interfaces table"注释已陈旧——接口表v2.4/v2.7已在线）→消费侧int32占位（CompiledModuleStubSupport.hpp:230 fallthrough）；泛型实例化签名序列化为擦除模板行（TypeDesc.cpp:398-401）；CompiledClass无implements字段（边仅存于源级SnClassDecl，实例化时经SubstitutedEdgeTypeArg重铸**接口边**，ExprResolverGenericInstantiation.cpp:190-266）。TypeDesc形状=kind u8+typeIdx u16+rank u8+typeParamOrdinal u8+elems（TypeDesc.h:73-85）；RemapTypeDesc只带struct/class两映射（TypeDesc.h:115-117）；ValidateTypeDescIndices只验struct/class界（:104-108）。模板尾isTemplate+typeParamNames已在线（v0.13.1 D7）。
- 条7现状：Type泛型臂只绑IdentifierExpr头（nlang.y:2244-2245及姐妹式），QualifiedType无泛型后缀臂（2292-2312）——`lib.Pipe<int>`在参数/返回/new等无歧义位同样语法即拒。语句头经HeadType（2314-2399）：**裸名泛型局部声明今天已可解析**（双尖括号臂2329-2353），缺口=点分链+泛型后缀（MemberExpr臂2355-2375无泛型形态）。ImplementsName泛型臂也只绑IdentifierExpr头（1842-1851）——`implements lib.I<int>`今天同样不可解析。as位泛型臂同样裸名only（2518-2546、AsGenericTarget 2574-2579）。泛型臂不共享TypeName位置的历史原因是`as y < z`比较归约（约束注释1827-1839），但**as位的比较拼写已死**：v0.13.1 D5的AsGenericTarget plain-'<'臂以shift默认拿走了该状态（2026-11-15探针实测`x as y < z`语法错误、`x as y`语义错误即解析通过）；且该比较语义上本不成立（`as`产出引用类型、'<'需要数值），不作恢复。裸名声明走plain-'<'臂的前提是**头状态独占**（台账2186-2190："comparison reading loses only in these head states—never in pure expression states"）——点分链的归约状态是全语境共享的（MemberExpr everywhere），这是D7.1b必须换机制的原因。关系算符%left结合（nlang.y:480）——**链式比较今天合法**（`(a<b)>c`），扫描器自主探针不设位置作用域就会破坏`f(a.b<c, d>e)`实参与初始化器链式比较。扫描器既有机制：自主形状探针ProbeTypeArgumentOpener（ScriptScanner.cpp:258-283，'<'后ScanAngleBody平衡体+终结符'.'/'('判定→m_bPendingReceiverLt）、强制比较旗标m_bForceComparisonReads（片段语境跳过探针）、IncGenericDepth中置动作先例。TypeName（':'与as共享，1805-1813）：点分链可自由归约的唯一前导token位，语句头不可用（1805-1811注释明证）。冲突台账2155-2217（2026-11-15实测32sr/5rr；勘误——本稿计划期写的30sr/3rr是2026-11-13的量，真合并14e824dd（2026-11-14）带入+3sr/+2rr（const头族、泛型头空括号、as位'<'臂）且未记台账，Task 2重测时已补记并全表重推；状态号随文法改动漂移，只可重测不可引用）。
- 条8现状：拼写仍走v0.12.3裸名裁定——**泛型模板行与实例化alike键BaseName**（TypeArgSpelling.cpp:123-133）；非泛型类行键本来就是包限定（"thread.Thread"，CompiledModule.h:448-452注释）。LinkKeyOf同时供行键与描述符拼写（契约注释118-121：两键必须同源防漂移；Register.cpp:44-53 VmBackend::KeyOf=LinkKeyOf；ClassArgOf:192/InterfaceArgOf:213普通臂也走它）——单函数契约是条8必须先拆的对象。运行时CanonicalSpelling原样拼写所给名（GenericInstDescriptor.cpp:22-35）；加载侧InternDescriptorFromSpelling叶子经FindStruct/FindClass/FindEnum解析（199-230）——FindClass是精确串匹配（CompiledModule.h:441-446），未解析叶静默降级rowRef=-1（GenericInstDescriptor.cpp:120-124,158-159）；带往返校验（221-228）。DescriptorRowIndexFor行名取BaseName（DescriptorSpelling.cpp:84-133）。D2设计目标=全限定链接身份（generic_runtime_typeinfo_design.md §D2）。
- 条9现状：ClassInheritOpt=':' TypeName无泛型臂（nlang.y:1802-1813）；铸造侧CreateInstanceShell逐字拷贝tmpl.SuperClass()（:109）、FillInstanceWindow刷新（:176）——泛型基类时实例拿到的是**带未绑定占位符的基类模板边**；SubstitutedEdgeTypeArg只覆盖接口边（:190-214）；线上superClassIdx指模板行（RegisterClass.cpp:150/257）。

# 1. 主流参照（2026-11-15取证）

- JVM：擦除+Signature属性——泛型签名以字符串文法附着于类/字段/方法的**声明位**，运行时不参与实例化；泛型基类的实参绑定记录在子类签名中（getGenericSuperclass、Super Type Token惯用法）。对应启示：签名即拼写，声明位携带、消费侧解析；"行键擦除+声明位签名串"正是条8双键分离的主流先例。
- CLR（ECMA-335）：TypeDef（定义，开放泛型）与TypeSpec（使用处实例化，签名blob行）分表；InterfaceImpl表以类型引用行承载"实现某接口"边。对应启示：定义与实例化拼写分键空间、实现边=类型引用清单。
- 可见性：模块/脚本语言生态主流默认可导入（Python模块属性、JS导出面、C外部链接）；大库工程语言默认非公共（Java顶层包私有、C# internal、Rust/Go私有）。NLang定位=类型安全脚本语言，stdlib是策展面、消费方是脚本——顶层默认public与自身生态定位一致，封装用显式private/protected表达（D5.1裁定依据，独立于现状）。
- 文法歧义消解：javac语句头`A<B> c`与`a < b > c`的消解（**仅在语句头位置**尝试声明读法，实参位恒为比较——有界回溯手写解析器）是D7.1b头作用域化探针的先例；C# "grammar ambiguities"探针解法本仓已在用（QualifiedGenericReceiver）；残余误报面两语言一致。

# 2. 条5：可见性访问控制

- D5.1默认规则：自由函数与顶层类型默认**维持FA_Public**（OnAddedToParent归约不动）。裁定依据独立于现状：模块/脚本生态主流默认可导入（§1），与NLang消费形态（脚本import策展stdlib）匹配；封装需求以显式private/protected表达。本条交付的能力=private/protected可拼写、线字节承载、跨模块强制执行——不是收紧默认。类成员默认维持现状（成员private）。
- D5.2作用域定义：FA_Private自由函数=仅本模块可见；FA_Protected=本模块+同包兄弟模块（今天包=模块，行为重合；线字节先行登记语义，点分多模块包线落地时激活区分）；FA_Public=全模块可见。类型级：FA_Public类型可导入；FA_Private/FA_Protected类型为包内类型——**成员机制=限定名查找的存在性过滤**：RegisterExternalStubs会为线上一切导入类型合桩（含私有类型），故排除发生在消费解析器的查找成员资格层（限定名解析对private/protected导入类型直接不命中，报"does not exist"族诊断），而非AllowAccess访问检查（后者管命中后的使用权）——两个钩子分工在手册与注释中写明。
- D5.3线格式：CompiledFunction追加u8访问字节（NCU 2.19→2.20，全量产物随版本重编，0.10.14先例）；加载侧桩合成按字节铺设，替换硬编FA_Public。类型记录（CompiledClass/Struct/Interface/Enum）各追加访问字节。
- D5.4类型级访问文法：类型声明族（class/const class/struct/interface/enum）加访问前缀位（AccessTypeNonEmpty|AccessTypeEmpty），默认FA_Public。自由函数与顶层类型默认一致（都public），OnAddedToParent的NK_Namespace臂无需按节点种型分支。
- D5.5执行点：仅编译期。**钩子=任何解析落在跨模块桩上即触发访问检查**（桩属主模块≠消费者主模块即跨界）——同时覆盖非限定名绑定路径（桩在Root()->Members()，CompiledModuleNodeBuilder.hpp:151-163实测）与限定名调用路径。本模块内部零开销不查。加载器不检查（维持现状）。protected对类成员维持既有声明类超类链语义，对自由函数/类型为包界语义——两个语义并存并在手册中分述。
- D5.6 stdlib审计：默认不变更，无重写面。审计内容=(a)设计复核——stdlib自由函数面确无应收敛为private/protected的成员（策展判断）；(b)产出「stdlib公共函数面」清单供手册核对；(c)显式新增private/protected的stdlib成员（如有）过全量e2e快速失败。
- D5.7验收：e2e新族（源内联测试包显式拼写private/protected）——导入private/protected自由函数拒绝且诊断具名（限定名与非限定名两条绑定路都钉）、导入private类型限定名拒绝（存在性过滤位）、public照常、protected同包可用；伪造产物装置（D6.4）注入越权访问字节验证加载侧不越权。

# 3. 条6：接口与泛型类型签名完整化

三处同根：TypeDesc叶不承载名字（接口降级、实例化擦除、实现边无位）。统一修法=名字承载的TypeDesc叶族。

- D6.1接口叶：TypeDesc新RTK_Interface种类，**编码钉死=复用既有u16 typeIdx字段载interfaceIdx**（索引该单元CompiledInterface表；TypeDesc.h:73-85形状不变，不加新字段）→表已在线v2.4/v2.7（含方法签名/静态/常量）；TypeDesc.cpp:294的陈旧注释随本片更正。消费侧SynthTypeExprFromDesc按接口记录键重建SnQualifiedTypeExpr（NamedTypeExprFromKey通道），替换int32占位fallthrough。
- D6.2实例化签名叶：泛型类实例化的TypeDesc新种类，**typeIdx载描述符行引用**（v2.15描述符表已在线），消费侧按行拼写重建SnGenericTypeExpr——复用加载侧InternDescriptorFromSpelling的拼写解析算法形状做编译侧重建（编译侧禁止intern，仅拼写→AST，stale rowRef禁令维持）。内建List/Dict/Array臂保持既有重建路径不动。编译侧重建不面对内联库头部（三轮核实：桩合成只对导入的已编译包发生，内联源贡献真实AST永不走桩；重建只产出拼写AST，头部解析归消费方自身导入表——D7.3路径），无需编译期头部校验。
- D6.3实现边线清单：CompiledClass追加implements边数组，每边=一个TypeDesc叶（D6.1接口叶或D6.2实例化叶）。加载侧桩重铸边；消费侧解析复用7fac1dcc替换机制（SubstitutedEdgeTypeArg）。接口静态分派/`as`接口检查消费该边。
- D6.4伪造产物wire尾测试装置：0.13.3审查递延项随本条建设。形制=单测工具头（tests/test_support/）：手构CompiledModule→WriteCompiledModule→加载断言，templateFlag族/抽象方法守卫/访问字节/描述符行可编程注入；既有手构模块测试为样例（test_vm.cpp:115-146）。
- D6.5版本纪律与全链职责：条5/6/8/9共用一次NCU小版本提升（2.20），随首片落地推进；宿主ABI不提升（无新VM回调）。**新RTK种类的全链职责（评审补全）**：(a)写入端BuildTypeDesc新臂；(b)解析端ParseTypeDescBytes新种类+ValidateTypeDescIndices扩界（interfaceIdx<接口表行数、rowRef<描述符表行数）；(c)合并端RemapTypeDesc扩臂（interfaceIdx走接口表映射、rowRef走描述符映射，.at()响败维持）；(d)消费端桩re-save往返不丢新种类。全链缺一即坏映像或静默错链。
- D6.6验收：e2e——导入函数接口类型返回值逐字重建（非int占位）、导入泛型实例化返回类型逐字重建、导入类桩implements边在位（接口分派/`as`接口双向）；**传递型暴露族（三轮补、四轮补边形态）**——只导入P而P的导出签名提及P自身导入的lib类型：签名叶形态（返回`lib.Pipe<int>`/接口签名）桩重铸处报具名诊断（点明补`import lib;`）、该叶降级占位（行为与今天持平但不再静默）；**implements边形态**（P类implements lib.ILogger）：删边+同一具名诊断（与今天无边状态平价，不崩溃不静默错分派）；正例对照=同时导入P与lib则逐字重建/边在位；伪造装置注入缺边/坏拼写/越界索引验证加载侧报坏映像而非错链。
- D6.7传递型签名暴露（三轮裁定、四轮补全）：消费者C只导入P、P的导出签名提及P自身导入的lib类型时，逐字重建叶`lib.Pipe<int>`在C的导入门内不可解析（ImportGateAllows拒绝，ExprResolverTypes.cpp:85-87）。裁定=**桩重铸处具名诊断+不可解析头降级占位**（评审方案a+b合并）：诊断在构建期点名缺失导入（可行动、无语言面变更），降级保住该成员的既有行为；其余签名成员照常逐字重建。四轮补全三点：(a)重铸侧可解析性判定**必须复用解析器自身的门谓词ImportGateAllows**（不得另实现一份——双判定源分叉即重现所避免的双重报告：重铸放行、解析器拒绝）；(b)诊断去重=每个导入包的重铸趟内按缺失头拼写去重、聚合受影响成员名（清单设上限），防一个缺失类型横跨多成员风暴构建日志；(c)**implements边的传递型暴露走删边+同一具名诊断**——边今天不在线上（CompiledClass无implements字段），"降级占位"对边无意义（int32边=非接口边），删边+诊断正是与今天无边状态的严格平价，D6.6补对应验收钉。传递型可见性作为语言特性（桩叶绕过导入门）超出0.13.8范围，登记为将来语言线候选（§8）——若立项须独立裁定（Java/C#的import仅为命名简写非访问门，NLang的门语义是存在性判定，两者取向不同）。

# 4. 条7：限定泛型拼写

- D7.1a无歧义位（Type规则）：加QualifiedType头的泛型后缀臂，镜像既有IdentifierExpr臂的双尖括号形制（OT_TypeLt臂+plain-'<'臂带IncGenericDepth中置动作，nlang.y:2244-2245先例），覆盖参数/返回/字段/new/类型实参位。TypeName本体不动（implements/as共享位维持裸臂）。
- D7.1b语句头（二轮评审裁定重设计——**扫描探针机制，头作用域化**）：裸名臂的LALR机制**不可原样扩展**到点分头（点分链归约状态全语境共享，台账2186-2190；加臂即`a.b<c`全语境Type-vs-Expression归约冲突，javac的有界回溯不可移植到yacc状态机）。机制=**解析器馈送的语句头扫描模式+自主形状探针**：(a)Statement入口空中置动作武装扫描器并**复位链跟踪状态**（四轮补：跟踪态跨语句存活则前语句`a.b;`的陈旧链+后语句`x<y>z;`过深度门误入判定路径——语句边界即干净态；parser-fed旗标先例：OT_TypeLt扫描标志）；(b)武装期间扫描器跟踪`ident('.'ident)*`链（**深度≥2才武装判定——裸名头维持今天plain-'<'路径零行为变化**）；(c)链后遇'<'时跑平衡体扫描（ScanAngleBody先例），终结符∈{标识符,'['}→发OT_TypeLt（HeadType的MemberExpr头OT_TypeLt臂接住），否则发plain-'<'并解除武装；(d)m_bForceComparisonReads片段语境照旧跳过。**武装旗标须无条件消费**（不泄漏进后续比较'<'——nlang.l:384-387既有无条件消费纪律先例）；链跟踪实现配料全有仓内先例（{Name}规则消费一次性旗标m_bPendingClassDeclName，nlang.l:413-422；平衡体扫描+行列数学=ProbeTypeArgumentOpener/ScanAngleBody/SkipBlanksAndComments族，ScriptScanner.cpp:258-283）。**武装面覆盖附注（三轮）**：文件级变量是远期登记面（roadmap：文件级变量与main并存）——若其产生式不经Statement，武装中置动作需在生产族加孪生（实施计划核对项）。位置作用域的必要性（实测钉死）：关系算符%left链式比较合法——不设作用域即破坏`f(a.b<c, d>e)`实参与初始化器链式比较；头作用域后二者都不经过武装态（前者头token后'('破链，后者头token是关键字）。残余误报=`a.b<c>d;`形态的标识符引领表达式语句（链式比较语句，语义无值）误读为声明——javac同一残余，接受并登记风险（§8）。bison成本=Statement一个空中置动作（epsilon产生式），过D7.2重测。
- D7.1c implements位：ImplementsName加QualifiedType头的泛型臂（同双尖括号形制）——`implements lib.I<int>`（导入泛型接口）随本位可解析，即D6.3的消费面。
- D7.1d as位：AsGenericTarget加QualifiedType头臂（既有裸名臂2574-2579同形制）——`x as lib.Box<int>`。bare头的`as y < z`比较拼写已死（§0探针实锤，语义本不成立），点分头同型无新增损害。
- D7.2台账约束与重测程序：bison冲突维持既有量级（2026-11-15实测32sr/5rr——计划期的30sr/3rr已被真合并漂移取代）不新增rr；新增sr须逐条入台账注释。**台账状态号随每次文法改动漂移（台账自注），本稿引用的状态号只作历史坐标；重测程序=每片落臂前后各跑一次完整冲突台账生成、diff增量逐条裁决——禁止引用绝对状态号作验收。**
- D7.3消费侧：限定泛型拼写→对导入模板做实例化（与D6.2拼写重建、RejectUnimportedQualifiedType既有拒绝位衔接：先存在性后泛型臂）。
- D7.4验收：e2e——`lib.Pipe<int>`变量（语句头）/参数/返回/字段/new/implements/as全位、嵌套`lib.Box<lib.Pipe<int>>`、跨包同名模板限定拼写消歧（与条8验收族共享proj_generic_cross_unit扩展）。

# 5. 条8：描述符规范拼写改包限定链接身份

- D8.1双键分离（评审裁决落定）：**行键与描述符拼写分函数**。LinkKeyOf原样不动（行键+VM表键空间：泛型擦除裸名、prelude裸名、非泛型类包限定，全部现状）；新增DescriptorSpellingOf作为描述符键空间的唯一来源：泛型模板与实例化=全限定链接身份（根包类型即裸名；包路径取registry路径派生，与SymbolImport.modulePath同源——产消两侧在链接闭包内自然收敛于目标模块点分路径）。消费位点枚举：DescriptorRowIndexFor行名、ClassArgOf/InterfaceArgOf的全部分支（类型参数占位符与标量臂除外——位置编码不动）、嵌套实参递归、**FreeCallDescriptorIndexFor**（DescriptorSpelling.cpp:139-147；EmitExprCore.cpp:355今天传callee->Name()=裸名——泛型自由函数行跨包同名折叠进intern池，随本片改传函数的限定链接身份，prelude仍裸名，与模板行同规）。组合记录字节内嵌rowName（DescriptorSpelling.cpp:91-93）——rowName文法变化即组合字节整体变化，**不做字节稳定**（单次版本提升全量重编）。TypeArgSpelling.cpp:118-121的单函数契约注释随本片改写为双键契约（各自键空间内单源、两函数禁止互串键空间）。
- D8.2行键不动（表述修正）：泛型模板行键=裸名维持（v0.12.3擦除模型；ClassSlotFor裸名查找、TypeDesc.cpp内建裸名比较、桩名匹配、MergeClasses全部不感知）；非泛型类行键=既有包限定（"thread.Thread"）不动。合并算法零改动。
- D8.3加载侧限定头解析（评审裁决落定）：拼写二分——**裸名=根包/prelude，走既有精确匹配路径不动；点分头=按最后一点切分，头部包路径对有效头部集校验，叶名仍走链接表既有裸名解析**（擦除模型下泛型模板行合并去重、构造上无歧义；类型同一性由限定拼写承载而非行引用——正是D2的分层）。**有效头部集（二轮评审补全）**=被链接模块自身modulePath（CompiledModule.h:337"compile unit's package identity"）∪其SymbolImport.modulePath（:382-383）∪**nlink填充的全部合并单元包路径集（运行时专用、不上线的字段——先例：CompiledModule.h:422"Runtime only, set by nlink after linking"）**。第三项的理由：源内联库TU（同名兄弟.n搜索路径发现、并入同次编译）属**不同包**且**不是导入**——其模板拼写限定头既不在自身路径也不在导入表，缺第三项则内联库泛型的首次cast/new即运行时硬错误。解析发生在VM执行期（VmExecutorOpsDescriptor.cpp:42族）。**头部不命中有效集=硬错误（坏映像），废除静默rowRef=-1降级**（GenericInstDescriptor.cpp:120-124,158-159的降级路径随本片改为抛出，消费侧重建对未解析行同硬败——与D6.6验收对齐）。往返校验（221-228）保持硬校验。
- D8.4漂移防护与验收：防护三层——(a)D8.3硬解析（写侧拼错→加载即坏映像）；(b)往返校验维持；(c)e2e跨包同名模板族（proj_generic_cross_unit扩展：不同包同名模板typeName互不相同、经Object跨包转型按限定拼写失败、静态字段补偿模式键区分；**源内联库+限定泛型cast/new族——二轮评审补，proj_generic_cross_unit永不覆盖此形**）。序列化往返单测（描述符表手构+重载逐字对拍）。运行时CanonicalSpelling原样拼写所给名——行存限定拼写即运行时身份限定化（跨包cast区分的本意）。

# 6. 条9：类继承泛型实例化作基类

2026-11-15用户裁定推翻死端定性：不支持`class D : Box<int>`本身不合理，本版交付（roadmap条9载裁定全文）。

- D9.1文法：ClassInheritOpt改挂新非终结符InheritName——**镜像ImplementsName的独立非终结符解法**，不共享TypeName（':'位无表达式竞争，1805-1811注释明证）：头=TypeName（NameExpr|QualifiedType，点分可达）+双尖括号泛型臂（复用条7机制族形状）。三形态：`class Sub : Box<int>`（具体实参）、`class Wrap<T> : Box<T>`（自有参数作实参）、`class Sub : lib.Box<int>`（跨模块）。
- D9.2语义裁定（0.13.1概要设计D1遗留裁决，就此定案）：描述符实参位=派生类自有声明参数按序占位；基类实例化=把自有参数代入基类拼写**推导**而得（Wrap<int>⇒基=Box<int>），不做基类槽与自有槽的平面合并。对象位布局=基类实例化字段代入后代入派生类自有字段（沿既有字段偏移行走），按位读取走Box<int>自身的行（superClassIdx可达）。
- D9.3抽象成员补全（原"死端"关切的正解）：补全发生在**派生类**的实例化处而非基类实例化处——具体类Wrap<int>继承含抽象成员的Box<T>必须提供实现，否则Wrap须自身抽象；模板形态Wrap<T>维持未绑定态规则（0.13.1c既有谓词）。implements边的实例化替换（7fac1dcc）与抽象守卫沿用。
- D9.4引擎机制（评审确名=新机制，非纯拼写推导）：(a)**超类边替换**——实例化处的superClass边照SubstitutedEdgeTypeArg形制替换：泛型基类边=基类模板+实参（自有参数代入）→**经实例缓存递归实例化**，Wrap<int>的SuperClass()=共享缓存的Box<int>节点（类型同一性，不得旁路缓存新造节点）；(b)**普通类解析布线点**——`class D : Box<int>`非泛型派生在普通类解析处走同一替换入口；(c)线上superClassIdx仍指基类模板行，基类实例化实参经TypeDesc叶随类记录行上线（D6.2机制复用）；加载侧LinkSuperclassStub按实参重建（消费侧可见Wrap<int> IS-A Box<int>全链）。
- D9.5运行时：Sub→Box<int>转型走既有实例化感知cast（38016179族）；组合记录承载基类实参的可达性由描述符行保证。
- D9.6验收：e2e钉死三形态——布局按位读写（基类泛型字段经派生对象）、`as Box<int>`成功与`as Box<long>`失败、抽象补全缺位具名诊断、跨模块继承形态（与D6验收合流）。

# 7. 实施切片依赖与顺序（概要）

条5（独立：线字节+文法位+审计+执行）→条8（拼写身份，为6.2铺路）→条6（叶族+线清单+伪造装置）→条7（限定泛型文法族）→条9（依赖6.2叶与7.1机制族+D9.4引擎替换机制）。每片独立可测、独立提交；线格式单次提升（D6.5）随条5首片推进。

# 8. 风险登记

- bison冲突漂移（条7/9文法臂）：按D7.2重测程序控盘，冲突增量逐条裁决，绝对状态号不作验收。
- 类型声明访问前缀位（D5.4）：与自由函数前缀共担AccessType归约的follow集（nlang.y:710-781既有同串双推导冲突注释），新增epsilon归约位须先过冲突台账核查再定形制。
- 双键漂移（条8）：行键/拼写两函数并存的新漂移面——防护三层（D8.4）；两函数禁止互串键空间（注释契约+单测钉跨包区分）。
- stdlib面（条5）：默认不变更无重写面；显式private/protected新增（如有）走快速失败（编译期/e2e即红）；不设兼容垫层（不向下兼容）。
- 编译侧描述符拼写重建（D6.2）不得触碰intern池（stale rowRef禁令，GenericInstDescriptor.h:44-57）；重建只产出AST拼写叶。
- 加载侧硬错误化（D8.3）：废除静默降级后，存量非法拼写产物直接坏映像——单次版本提升全量重编，无兼容负担。
- 条9对象布局：泛型基类字段替换的偏移行走须与既有LinkSuperclassStub偏移模型一致；布局回归用位读写e2e钉死；超类边替换经实例缓存（类型同一性），不得旁路缓存新造基类节点。
- 条9循环基类链（`A<T> : B<A<T>>`+`B<T> : A<B<T>>`）：经既有实例化深度上限守卫终止（通用"嵌套实例化超限"诊断，每模板每构建去重一次）——语义即循环继承（C# CS0146具名诊断先例）；专用循环基类诊断是否加设归实施计划裁决，本版以深度守卫终止为可接受。
- 条7头探针残余误报：两种形态接受为声明读法（javac同一残余面，语义均无值）——`a.b<c>d;`（标识符引领链式比较表达式语句）与括号形`a[b<c>[0]]`族（链式比较接下标）；实测形态若出现即诊断点明"声明读法"，不静默。
- 传递型可见性（D6.7将来候选）：桩叶绕过导入门=语言面变更（import从存在性门变为命名简写=Java/C#取向），超出0.13.8；若将来立项须独立裁定并走规格流程。

# 9. 主流参照出处

- JVM Signature属性：JVMS §4.7.9（类/字段/方法级签名文法，声明位携带）；Super Type Token=Neal Gafter惯用法（Gson TypeToken实现）；"行键擦除+声明位签名串"=双键分离先例。
- CLR：ECMA-335 Partition II（TypeSpec、InterfaceImpl、GenericParam表；实现边=类型引用行）。
- 可见性：Python/JS模块默认可导入、C外部链接；Java顶层类默认包私有、C#默认internal（程序集界）、Rust/Go默认私有——NLang按脚本生态取可导入默认（D5.1裁定依据）。
- 文法歧义：javac语句头`A<B> c`消解（'>'后标识符即声明读法）；C# "grammar ambiguities"探针解法（本仓QualifiedGenericReceiver已在用）。
