# WEB框架实施期间的语言面缺口清偿设计

概要设计（spec）。锚定提交`d320da6b`（分支`dev`，VERSION 0.13.13）；事实面探针`temp/gaps_probe/`与`temp/lam_probe/`，判决见`temp/gaps_probe/results.log`。交付版本**v0.13.13a**（用户裁定，2026-11-26）。逐文件改动清单、文法产生式与bison冲突核对、门禁批次切分属实施计划，另文。

## 一、问题与目标

roadmap「WEB框架实施期间的语言层面缺口」五条（`docs/roadmap.md`远期特性，2026-11-19实测登记，探针坐标与初判修法在`docs/dev/web_framework_design.md`九.18）同属一族：**默认表达式与构造实参位没有把形参声明类型作为期望类型接进去**，外带两个独立小面（容器元素就地调用、构造器默认值的静态作用域）。

目标：按合理性优先（现代主流语言的通行做法、逻辑自洽、程序员易用）一次清偿五条；不向下兼容（2026-09-14全局裁定），不为迁就框架侧既有改写形制收缩语义。框架侧的绕行改写（局部变量中转、`MemorySessionStore`三档构造器重载）在语言面落地后回改，属后续框架批次，不在本spec范围。另按缺口4附注（「配套`Set.contains`的风格一致性一并核」）与用户授权（2026-11-26「需要的话，请实现Set容器」），本批一并交付基础容器`Set<T>`（D-6）。

## 二、事实面（2026-11-26于`d320da6b`全量重建后复测）

| 缺口 | 拼写 | 现状 | 根因位点 | 对照腿 |
|---|---|---|---|---|
| 1 | `func<void, Ctx> cb = noop`；`cb = (Ctx c) -> { }` | 声明侧默认表达式被拒：`function reference "noop" requires an expected function type.`／`a lambda expression requires an expected function type.` | `StatementResolverDeclDefaults.cpp ResolveFormalDefault`调`Resolve`不带期望类型 | 同一引用显式传参（含其他形参带默认值的场合）正常（g1a/g1b PASS） |
| 2 | `App(noop)`；`App(lg.note)`；`App((c) -> {})` | 构造实参位函数引用被拒（裸拼写报引用期望类型错），lambda同拒（旁注：`new App(noop)`也报语法错，但那是`new`自0.9.0退役为普通标识符的两标识符并置错误，与本缺口无关） | `ExprResolverNew.cpp TryResolveCtorInvoke`与`BindQualifiedCtorTail`跑在实参已按无期望类型解析完之后，无绑定通道；泛型实例化拼写`Type<Args>(args)`走同文件`Access(SnNewExpr&)`（实参同样先于构造器选定解析，见D-2；该拼写现状在探针中无钉，计划期先钉）；普通调用成功尾`ExprResolverInvoke.cpp BindArgFuncRefs`是现成范本 | 先落带类型局部再交构造可行（p7 PASS） |
| 3 | `mws[0](1)`；`fns[0](1)`；`ds["one"](1)`；`(cb)(1)` | 一律parse级`syntax error`；`mws[0].invoke(1)`能解析但func面无`invoke`成员 | 文法无「表达式接实参表」臂：`nlang.y InvokeExpr`只接标识符，成员访问与下标已是通用后缀臂 | func类型局部直接调用正常（p1 PASS＝函数值间接调用产码先例） |
| 4 | `d.get("zzz")`缺键 | VM异常`NLang VM: Dict key not found`（退出码1），无缺省安全取法 | `stdlib/containers.n`的`Dict`面=set/get/containsKey/remove/clear/count/keys/copy/toString | — |
| 5 | 构造器形参默认值`= K`（本类裸）、`= CfgQual.K`（本类限定）、`= Knob.K`（他类） | 三拼写全拒，共用误导诊断`…cannot reference 'this' …where no valid 'this' exists.` | `StatementResolverDeclDefaults.cpp ReferencesThis`把任何绑到类/结构字段的标识符一律算this引用，不分裸/限定拼写、不分静态/实例 | 同一`public static const int K`作**方法**形参默认值正常（g5d PASS） |

补充事实：模块作用域只有函数与类型声明，没有文件级常量声明（`const`只修饰类，HTTP线旧缺口「文件级变量不能与`main()`并存」在册，本批不动）；全仓当前无`Set`容器（本批随D-6交付，用户授权2026-11-26）；`foreach`源是封闭集——array、List、Dict、string（`StatementResolverFlow.cpp:257`诊断自认），容器族之外一类都不接；`Dict`保插入序（声明面注释自认「insertion-ordered keys」），`Dict.keys()`返回`List<K>`。

## 三、业界先例

- **C++默认参数**（[dcl.fct.default]）：默认实参中非静态类成员与`this`一律禁用（"Non-static class members are not allowed in default arguments (even if they are not evaluated)…except when used to form a pointer-to-member or in a member access expression"），静态成员可用；默认实参在每次调用的调用者上下文求值。为缺口5的「静态可用、实例拒」提供直接先例。
- **Kotlin默认参数**：构造器默认表达式不能引用本实例成员（计算发生在实例初始化之前），官方推荐伴生对象/顶层函数作默认值来源；成员函数的默认值则可在实例作用域求值。与NLang方法/构造器默认值的既有不对称同形。
- **Python**：调用是作用于任意atom/primary的trailer，函数是一等值，`f(a)(b)`、`d[k](x)`天然成立；调用本身是表达式。
- **ECMAScript**：ES5.1 §11.2 `CallExpression : MemberExpression Arguments`（`MemberExpression`递归展开即后缀链、涵盖主表达式）——调用与成员访问、下标同属一条后缀链，不限于标识符。Kotlin/C#的调用形态同为「任意值得到类型后可后缀调用」。
- **缺键默认取法命名对照**：Java`Map.getOrDefault(key, defaultValue)`（Java 8）、C#`GetValueOrDefault`、Python`dict.get(k, d)`。三者语义一致：命中返值、缺键返默认、不插入、不抛。
- **基础容器三件套**：List/Set/Map（`Set`在Java`LinkedHashSet`、C#`HashSet`、Python`set`皆为与List/Dict并列的基础容器，`add`/`contains`/`remove`是通用方法面）；NLang预置包已有List/Dict，Set是缺失的第三件。映射型容器以既有映射结构承载（如Java`Collections.newSetFromMap`）是公认的低成本形态。

## 四、逐项裁决

### D-1（缺口1）默认表达式沿形参声明类型传播期望类型

- **裁决**：`ResolveFormalDefault`把形参声明类型作为期望类型传入表达式解析，函数引用与lambda表达式在默认表达式位即按既有实参位的同一绑定/延后解析通道处理。适用于函数、方法与构造器的全部形参默认值。
- **理由**：失败仅发生于默认表达式位而显式传参正常（事实面缺口1），说明缺口只在「期望类型没接上」这一根线，与`ResolveDeferredArgs`（lambda实参按期望函数类型延后解析）同源，一条修法即可。每次调用求值默认表达式（lambda每次新建闭包）与Kotlin语义一致，可接受。
- **否决的备选**：要求用户先落带类型局部再写默认值——把编译器缺口转嫁给用户；为默认表达式另设专用解析通道——重复既有期望类型机制，违KISS。
- 被引函数带默认参数的既有拒绝门（`function reference … requires an expected function type`之外的`functions with default parameters cannot be referenced`）与本批正交，不变。
- 默认表达式内lambda的捕获与产码受§五跨缺口不变量约束（声明处解析、各调用点产码、lambda体内前向引用仍限更早形参），display填充由计划期探针先钉。
- 导出声明的默认表达式沿NCU默认值序列化通道交付、消费方stub构造期即重建，现行跨模块子集是封闭枚举——本批新增形态必须随批扩充，否则允许面在导出形下坍缩为仅同模块；裁决见§五「跨模块默认值序列化面」。

### D-2（缺口2）构造实参位补函数引用/lambda绑定通道

- **裁决**：构造器选定之后（三种构造调用文法拼写全覆盖：裸拼写`Type(args)`的invoke回退臂、限定尾巴各臂、泛型实例化构造臂`Type<Args>(args)`——后者产`SnNewExpr`自`QualifiedGenericReceiver '('`产生式（`nlang.y:2983`），走`ExprResolverNew.cpp Access(SnNewExpr&)`：该路径在构造器选定之前就把全部实参送入`ResolveExpressionList`按无期望类型解析、后`SelectCtorForArgs`选构造器——连pending函数引用/lambda也以已解析（或已报错）形态进入选定，恰与invoke回退臂「pending实参不以已解析形态进入选定、选定后才绑」的形状相反（invoke臂的常规实参同样先于选定解析，两条臂的差别只在pending形态的去向），绑定通道须在该位点让pending函数引用/lambda延后到实例化构造器选定之后按形参期望类型解析；`TryResolveCtorInvoke`的declared-generic臂只管裸名泛型调用（`Box()`沿赋值目标提示取实例），与此拼写无干），对未绑定实参按所选构造器的形参声明类型执行与普通调用成功尾等价的绑定：pending函数引用绑定到形参期望类型，lambda按形参类型延后解析。选定多构造器重载时沿用既有重载决议先行，绑定发生在选定之后。`new Type(args)`拼写不在清偿面：`new`自0.9.0起是普通标识符（`tests/e2e/err_new_removed.n`钉死），两标识符并置本身即语法错，与绑定通道无关，D-3后缀臂落地后该拼写仍是语法错。
- **理由**：同一枚引用在普通调用实参、字段赋值、带类型局部三处都能绑定，唯独构造实参位不能，是无对等通道的遗漏而非语义设计；构造器一旦选定，形参类型即期望类型，机制与`BindArgFuncRefs`完全同构。
- **否决的备选**：在invoke解析失败前预先猜测构造重载并预绑实参——把重载决议做两遍，且猜错路径诊断会变差；继续要求「先落局部再构造」——同D-1的转嫁。
- **回退门放宽**：构造回退现由`ResolveFailedInvoke`的实参可容门把守（`ExprResolverInvoke.cpp:307` `bArgsAdmissible`），p3的`The function "App" does not exist`即由该门拦下；放宽该门须把pending函数引用与lambda认作合法实参形态，Array维推断例外（`TryInferArrayDimsDeferredArgs`）的既有语义不得丢失。
- **失败路径诊断**：构造实参位传入签名不符的函数引用仍拒，诊断归引用族、点名该引用与目标构造器形参位的签名差异——通道打开后不得继续落`requires an expected function type`（该措辞意为「无期望类型」，届时已成谎报），也不得被重载打分淘汰埋进「无匹配构造器」族（根因是引用不符而非重载集）。锚定原则：错形引用使唯一或最接近候选落选时点名该候选；无从锚定时回落「无匹配构造器」族并列出该引用形不符的原因片段；pending实参参与候选打分按「形参位为函数类型即可容、其余照旧」执行。

### D-3（缺口3）调用并入通用后缀链

- **裁决**：文法把`(实参表)`加为后缀链的一环，与成员访问、下标同级，作用于任意前缀表达式；被调方（callee）的求值结果必须是函数类型值，否则报新诊断「表达式不可调用」（措辞终稿属计划）。`mws[0](1)`、`fns[0](1)`、`ds["one"](1)`、`(cb)(1)`、`f(x)(y)`均可写并自然左结合成链。产码复用既有函数值间接调用通路（func类型局部直接调用的先例），不新增opcode。不加`.invoke`成员。
- **优先级裁决**：`a.b(args)`维持方法调用解读——成员名在类内解析到方法或静态方法时走现行receiver形状分派（`ExprResolverInvokeReceiver.cpp`按名分派族），后缀值调用语义不接管；新臂的值调用只作用于解析结果为函数类型**值**的前缀（局部、形参、字段访问、下标、中间调用结果）。接收者上同名方法与同名`func`字段并存时**方法胜出**，字段值须显式中转取得（`f = obj.f; f(1)`）；该组合钉单测腿，文法冲突消解同此取向——冲突台账只解决能否解析，此裁决决定解析成哪个。
- **理由**：主流语言一致把调用视作任意值上的后缀操作（见§三），NLang的成员访问与下标已是通用臂，唯独调用臂绑死标识符，属文法遗留而非语义限制；`.invoke`成员是给缺口打补丁（C#`Delegate.Invoke`形制），与「调用是语言操作不是成员查找」的既有模型冲突，YAGNI。类型不符的调用在解析期拒绝，不留到运行期。
- **否决的备选**：仅对下标结果开专用产生式——`(f(x))(y)`仍不通，规则反而更难解释；语句形态单开家族——同一表达式在语句/表达式两形态规则分叉，违「表达式优先」文法惯例。
- 风险登记：新臂与既有`InvokeExpr`/语句臂的bison冲突须在实施计划中以冲突台账核对收口，不许用悬空消解掩盖。

### D-4（缺口4）`Dict.getOrDefault`

- **裁决**：`Dict<K, V>`新增`public native V getOrDefault(K key, V defaultValue)`：命中返值，缺键返`defaultValue`；**不插入、不抛**，返回值按值（缺键路径上没有驻留槽位可取引用）。落点=`stdlib/containers.n`声明面（真源）＋`BindingTable.cpp`注册＋VM intrinsic实现；宿主代理面按`Dict.containsKey`先例核对是否需同步。`containers.n`是预置源（编译期内联进编译器，`CMakeLists.txt`包域表无containers、文件头自认「the stdlib packages exclude this file」），**不进任何`.npkg`、无重打包需求**；刷新纪律＝重建拥有者目标＋build-tree stdlib磁盘拷贝刷新（陈旧拷贝先例，FindStdLibDir命中处）。
- **理由**：三语言先例语义收敛（§三）；命名取Java形`getOrDefault`，与既有`containsKey`的Java风命名家族一致。既有`get`缺键抛异常保持不动——「缺键即错误」与「缺键给默认」是两种意图，各有清晰写法（需要判定时用`containsKey`），不必合并。
- **否决的备选**：`get(key, default)`重载（Python形）——`get`单形已定且返回`V&`，重载返回`V`会让返回类型随重载变化，声明面更浑浊；`GetValueOrDefault`（C#形）——与仓内既有Java风容器命名不一致。

### D-5（缺口5）构造器默认值按调用者作用域求值

- **裁决**：两事分明——**名解析**沿声明处上下文进行（与现状一致，`ResolveFormalDefault`的解析时机不变），**求值**发生在调用点（既有诊断文案自己申明的语义）。允许面＝在声明处上下文可静态解析、且绑定目标不依赖实例的一切：字面量与常量折叠表达式、任意可访问类的**静态**成员（字段与方法调用；本类裸拼写、本类限定拼写、他类限定拼写一律放行——三拼写在声明处上下文解析到同一静态成员；他类限定拼写的可见性随声明处可见的import而定，与调用点无关；导出声明时消费方的重建受§五跨模块子集裁决约束）、不依赖实例的lambda。拒绝面＝真正依赖实例的表达式：显式`this`、绑定到声明类非静态成员的访问，且沿lambda体与嵌套表达式穿透。诊断按实际拼写分措辞：显式`this`一句、实例成员访问一句（点名成员与所属类），不再对静态拼写谎报`this`。方法形参默认值的既有宽松（可在实例作用域求值）不变——与Kotlin的成员函数/构造器规则同形，属有意不对称。D-1通道叠加后，构造器形参的默认表达式`= noop`与`= (c) -> {}`同样成立（lambda体内不得穿透实例依赖）。
- **理由**：构造默认值在调用点求值（既有诊断文案自己申明的语义），此时实例不存在，「非静态成员不可用」正是C++标准规则与Kotlin构造器规则的重述；现状把三枚合法静态拼写一起误拒并谎报根因，是把防御做过头。修法是把`ReferencesThis`从「绑到字段就算」精确化为「绑定目标是非静态成员才算」。
- **否决的备选**：只放行限定拼写、继续禁裸拼写——本类裸拼写与限定拼写在声明处上下文解析到同一静态成员，禁前者无独立理由；给构造默认值加「仅限编译期常量」收紧——现状既有契约检查（前向引用/类型兼容/常量适配）已覆盖正确性，再加一道与主流相悖的收紧属无据收缩。

### D-6（缺口4附注＋用户授权2026-11-26）`Set<T>`基础容器

- **裁决**：`stdlib/containers.n`新增基础容器`Set<T>`，以**NLang源码形态**实现（非VM native）：内部持`Dict<T, int>`，方法面＝`add(T item): bool`（已存在则不插入返false）、`contains(T item): bool`、`remove(T item): bool`、`count(): int`、`clear()`、`copy(): Set<T>`、`values(): List<T>`（内部键表快照，保插入序）、`toString(): string`（沿用`values()`的列表格式）。成员判等与查询全部沿用`Dict`键机制，无独立语义——现行`Dict`实现为线性扫描O(n)而非哈希表（VM侧注释与手册12章「不提供O(1)哈希表查找」均在册），手册Set小节按12章同口径如实表述、性能描述不与「哈希」挂钩。基础容器入预置包（§3.9.4预置可见规则，恒可见）。`foreach`暂不接`Set`源——源封闭集扩一类牵动解析与产码两处，迭代走`for x in s.values()`已顺；是否纳入`foreach`扩展登记另议，不随本批。roadmap缺口4附注的「`Set.contains`风格一致性」随之落实：命名随`List.contains`家族。`add`返回`bool`（是否新插入）与`List.add`的`void`是容器族内有意的返回值差异（重复插入有可观察后果，与Java`Set.add`同形）；`remove`返回`bool`（是否删除，Java Set族同形）与`List.removeAt`的`void`、`Dict.remove`的`int`（删除个数）构成删除族的第三种返回值形态；`values()`命名对应成员快照而非映射键（Set非映射型，不套`keys()`）。三处族差异都在手册Set小节一并交代理由，本批不对齐签名。拼写归类裁决：`Set`入内建容器拼写族、保持裸名——`typeName(Set<int>)`=`"Set<int>"`，转型与判等一体遵行——即§4.12.9「内建容器保持裸名、用户模板带包路径前缀」规则句（`generics.md:171`，`formats/28-ncu.md:68`同引）的内建容器例外条款；该例外现登记面只含List/Dict，`Set`须同步进入例外位点，若机制对源码形态预置类强制冠`containers`前缀，计划期先探针钉死再定实现位点。
- **理由**：List/Set/Dict是主流基础容器三件套（§三），缺Set时「成员资格」写法只能借`Dict`半用或手写重复；NLang自己的裁定要求部分功能以nlang标准库形态交付而非全部native（AGENTS「新增或变更功能」），Set挂在已验证的`Dict`机制上：零VM改动、零新intrinsic、零GC/TypeDesc/序列化改动，用户可见的`.n`文件即真源。
- **否决的备选**：VM native `Set`（新运行时类型码`RTK_Set`）——类型描述符、GC扫描、序列化、`foreach`封闭集、宿主代理五处全链都要为新类型码开臂，为一个映射型容器付整套新kind的代价，不成比例；`Dict<K, bool>`手写惯用法替代——把标准库该消灭的重复摊给每个用户。
- **风险登记（计划期先探针钉死）**：①预置包内**非native泛型类**的声明与实例化路径预期与用户定义泛型同源（v0.12.0机制），若预置包语境（隐式import、包排除）有特殊门，须先钉再写；②预置类以源码进每个编译单元，确认按用词裁剪生效、未用`Set`的程序不背其字节码；③类类型元素的判等语义＝`Dict`同型键语义，实测钉死后写进手册，属特性表述而非限制。

## 五、跨缺口不变量与实现落点（概要）

统一原则一句话：**期望类型＝形参声明类型，沿默认表达式、构造实参、普通调用实参三条通道传播；调用是值的后缀操作。**

跨缺口不变量（D-1×D-5交叉面）：**默认表达式内的lambda在声明处上下文解析**——lambda体内的前向引用同样只允许指向更早形参（`CheckDefaultForwardRefs`的契约沿lambda体生效，否则正是其注释自认的调用点`FindLocal`崩溃家族）；产码沿**每个调用点**各发射一遍，display字段由调用点对应形参的实参cell填充（一节点、多调用点发射的形态）。**捕获面按宿主分治**：函数与构造器形参侧（调用点无实例可依赖）捕获面＝声明处可见的非实例名，实例依赖沿lambda体拒绝，与D-5构造器裁决同根；方法形参侧沿用方法默认值的既有宽松——**允许实例捕获**（`this`与实例成员，调用点receiver在场，display以receiver cell参与填充），与D-5「成员函数/构造器有意不对称」论证一致。该宽松的跨模块边界：方法stub与构造器stub同经`BuildStubFormalParamNodes`→`SynthDefaultExpr`在消费方stub构造期无条件重建默认值，而默认值编码节点集没有`this`/实例成员读取形态——一切读取实例的导出方法默认表达式都到不了消费方：既有非lambda标量形态（`= this.field`之类——`CheckCtorDefaultThisRefs`的this守卫只护构造器，方法形参一直放行——本批前就已落`RTK_Unfoldable`毒化import，行为不动、现状探针先钉）与D-1×方法宽松新开的实例捕获lambda同受此边界；新开的lambda形态以导出侧显式拒绝为处置（措辞属计划，实际编码回退形态由计划期探针先钉），即导出方法的默认lambda以**非实例捕获**形态跨界、含实例捕获者仅同模块成立；方法宿主的捕获腿随之分列——同模块实例捕获放行腿与导出含实例捕获拒绝腿各一。计划期探针先钉后写：`f(int seed, func<int, int> g = (x) -> x + seed)`于两处不同调用点调用、方法宿主捕获腿各一（§八）；捕获分析或display合成有洞即在计划层解决。

跨模块默认值序列化面（D-1×D-2×D-5与import机制的交互）：导出声明的默认表达式沿NCU`DefaultValueDesc`交付，消费方**stub构造期**对每个默认值无条件重建（`CompiledModuleDefaultSupport.hpp SynthDefaultExpr`，构造器stub臂`CompiledModuleCtorStub.hpp`同路），而现行跨模块子集是封闭枚举（字面量、形参引用、一元负与按位取反`~`、导出模块自由函数调用及其`+ - * / % << >> & | ^`双目——解码器`EDB_*`定位表全量在册（`CompiledModuleDefaultSupport.hpp kOps`）；`SynthDefaultExpr`拒绝文案自列的`+ - * / %`漏了`~`与按位六目、与在册子集不符，随批修正，见§六诊断文案面），子集外一律编码`RTK_Unfoldable`、消费方一import即记编译错——**即便调用方从不省略该实参**。不处理则D-1/D-5的允许面对导出声明坍缩为「仅同模块」，web包这类公开类的默认`func`形参会毒化全部下游import者。裁决：跨模块子集**随本批扩充**——函数引用（自由函数与静态方法按名引用、消费方侧重绑）、静态成员访问、**限定静态方法调用**（`ED_Call`现行只收导出模块自由函数、拒类/枚举接收者`EncodeCallDesc`；D-5允许面已放行该形态，跨模块须扩其接收者编码——自由函数调用早已跨界，单限此形态为同模块无独立理由）、lambda（体沿既有ExprDesc节点树序列化）四形态新增NCU内容编码，对导出声明同样成立；格式版本预期不动，内容面扩展如实写进§六ABI判定。分级回退：计划期若实证ExprDesc承载不了lambda体重建，仅lambda默认形态先限同模块（导出声明处显式拒绝诊断，措辞属计划），函数引用、静态成员访问与限定静态方法调用的扩充不受牵连；回退与否由计划期探针钉，不留给代码临场。

文件级落点（细节属实施计划）：
- `src/compiler/builder/StatementResolverDeclDefaults.cpp`——期望类型通道（D-1）、`ReferencesThis`精确化与诊断分措辞（D-5）；
- `src/compiler/builder/ExprResolverInvoke.cpp`、`ExprResolverNew.cpp`——构造器选定后的实参绑定通道（D-2）；
- `src/compiler/grammar/nlang.y`与解析器——后缀调用臂（D-3）＋bison冲突台账核对；
- 码生成——复用函数值间接调用，预期零新opcode（若计划期发现有洞以最小补全定稿）；
- `stdlib/containers.n`＋`src/compiler/builder/BindingTable.cpp`＋`src/vm/IntrinsicsContainer.cpp`（＋宿主代理按先例核）——D-4；
- `stdlib/containers.n`新增源码形态`Set<T>`——D-6，预期零VM侧改动；预置包不进`.npkg`（`containers.n`文件头自认「the stdlib packages exclude this file」），无重打包需求，但预置内联源改动后复跑语料前须确认编译器吃的是新源（陈旧守卫先例），计划期钉。

ABI：新增intrinsic若随`.ncu`/`.npkg`按id序列化，只表尾追加、既有id不动；NCU/宿主ABI格式版本预期不动，计划期核id序列化面后定稿。

## 六、用户可见面判定（手册同步基线五类）

- **语言面**：变更——默认表达式接受函数引用与lambda（D-1）；构造实参位接受函数引用与lambda（D-2）；任意函数值表达式可直接调用并入后缀链（D-3）；构造器默认值允许静态成员表达式（D-5）。
- **标准库面**：变更——`Dict.getOrDefault`（D-4）；新增基础容器`Set<T>`（D-6，预置包恒可见，手册集合章新增Set小节与§3.9.4预置可见面「即全部集合」枚举同步）。
- **诊断文案**：变更——构造器默认值拒绝诊断按拼写分措辞（D-5）；新增「表达式不可调用」（D-3）；构造实参位签名不符的函数引用落点名形参位的引用族诊断（D-2）；`SynthDefaultExpr`跨模块默认值拒绝文案补全在册子集（现列`+ - * / %`，漏`~`与按位六目，见§五）。
- **退出码**：不变。
- **ABI**：格式版本不动。两个内容面如实登记——NCU默认值序列化子集随本批扩充（新增节点编码，见§五跨模块裁决；不用新默认形态的既有语料字节面不变，与Set字节对拍腿同理）；`stdlib.npkg`内容**全不动**（`containers.n`预置源编译期内联于编译器二进制，不进包）。

手册同步清单（中英成对，`docs/user_manual/{zh,en}/specification`）：函数/闭包章（形参默认值形态与lambda/函数引用作默认值、构造器默认值作用域规则）、类章（构造调用的实参形态）、表达式/调用语法章（后缀调用链）、库参考集合章（`12-collections.md`现仅§12.1/§12.2，Set小节追加为§12.3、置于Dict之后——不插位改号，§12.2编号与其入向引用链全部原样；`Dict`面新增行；Set小节交代`add`/`remove`/`values()`三处族差异理由）。`Set`落地即过时、须同批改写的「List/Dict即全部集合」主枚举三处：`03-program-structure/declarations.md:137`（预置包声明枚举）、`03-program-structure/packages.md:149`（§3.9.4「即全部集合」句与恒可见名单）、`library/11-conventions.md:7`（同句）；同族句一并判定：集合章引言枚举句（`library/12-collections.md` zh:5「是两个内建容器类」/en:3-4「are the two built-in container classes」）、泛型章§4.12.6节题「内建容器List与Dict」（`04-types/generics.md:104`）与正文内建容器句（`:98`——结构体类型实参句须核Set同样成立）、`04-types/index.md:16`、以及内建容器拼写族规则句（`generics.md:171`、`formats/28-ncu.md:68`——按D-6裸名裁决随批改为List/Dict/Set三元组）；另`stdlib/containers.n:2`文件头注释所引锚点「§12.4」在现行specification树不存在（全树仅命中无关的`4.12.4`），按「过期注释当场改」纪律随批改为§3.9.4。本清单行号以zh侧为准（个别条目附带已核实的en镜像坐标），其余en镜像按句对句在手册批次现场定位。其中须点名移除的在册例外与限制：`07-functions/function-values.md`三处「构造调用的实参是例外」句（17、76、80行）随D-1/D-2转正撤销；限制节两条随之作废——「带默认值的`func`形参……声明本身被拒」（233行，D-1推翻）与「lambda字面量或裸函数引用作构造调用实参……修法排期见`docs/roadmap.md`的远期特性清单」（246行，D-2推翻，其中roadmap指针随之失效）；`04-types/struct.md`的`new Point(...)`幻影句（zh 45行/en 65行，`new`自0.9.0已退役为标识符，与e2e钉死规则矛盾）本批顺手修正。逐节坐标与措辞在手册批次以`tools/manual_sync_check.py`五类判定定位清零后刷`SYNC_BASELINE`。

## 七、版本与登记

- `VERSION`：0.13.13→**0.13.13a**（用户裁定2026-11-26）。字母标签先例两枚：`0.13.1a`（CHANGELOG 952行条目，其994行「Version strings may carry a letter tag (`0.13.1a`)」是字母标签机制的引入记录）与现役`0.13.12a`（CHANGELOG 17行，0.13.12补丁后补发的字母版、最近的现役先例）。历史版本号`0.13.1b`/`0.13.1c`已经用户裁定重编号为0.13.2/0.13.3（`docs/dev/v0137_merge_design.md`八节，master`72d3fda9`落账），CHANGELOG不再有对应小节，不得再引作版本号先例（旧号仅存于文法/源码历史注释，注释本身属实）。CHANGELOG新增`[0.13.13a]`段，中英成对。
- roadmap：五缺口条目改写为已清偿并指向本文档（缺口4附注的`Set.contains`风格项随D-6落实），HTTP线旧三缺口与其余远期项不动；`docs/dev/web_framework_design.md`九.18标注清偿去向。泛型完善（一）（二）（三）仍列v0.13.14/15/16，字母版不占用功能版本号、不随之顺延。
- 用户可见内建函数/方法有`.n`面文件：`getOrDefault`随`containers.n`声明面交付，满足。

## 八、测试策略（概要）

- **TDD**：每缺口先落失败测试再实现。单元层用真实编译器跑resolver断言（绑定成功/诊断原文）；e2e层把`temp/gaps_probe`与`temp/lam_probe`探针转正进tests语料。转正以全量重建后干净树复跑为准：g2b（`new App(noop)`）不转正——该拼写是`new`标识符退役的并置语法错，同腿改由裸拼写与限定尾巴形态承载；p8历史失败为nvm滞后于npkg格式1.1的陈旧树工件——2026-11-26全量重建后已复跑全部26枚探针，与旧log唯一差异即p8转PASS，判决全部落在`temp/gaps_probe/results.log`，按语言腿转正。
- **反向腿必配**（防过度放行）：构造默认值`= this.field`与非静态成员仍拒且新诊断点名成员；`= this`仍拒；非函数值后缀调用（`1(2)`、`"s"(1)`）报「不可调用」；`getOrDefault`命中与缺键双路断言，并断言缺键后`count()`不变（不插入）；构造实参位传签名不符引用——元数不符（`App((c, c) -> { })`）与返型不符（`App((c) -> 1)`）均交给转正语料`App`的`func<void, Ctx>`形参位（`App`构造器签名以探针源钉死，相符正形`(c) -> { }`已在p3在册）——仍拒且诊断按D-2锚定原则点名；`func<int, int>`槽下的相符正形（`(x) -> x`）由下述`Hold`腿覆盖、不可当反例；接收者同名方法+同名`func`字段并存时方法胜出的分派腿（D-3优先级裁决）。
- **默认值lambda捕获腿**（§五不变量，宿主分治）：自由函数宿主`f(int seed, func<int, int> g = (x) -> x + seed)`两处不同调用点各断言`g`用当次实参；构造器宿主lambda体内引用绑定实例成员的名字仍拒；方法宿主lambda捕获`this`/实例字段放行、断言用调用点实例（§五方法侧宽松）。
- **泛型构造臂**：`Hold<func<int, int>>((x) -> x)`构造实参位函数引用/lambda绑定（D-2第三种拼写，走线见其泛型臂段——绑定通道归`Access(SnNewExpr&)`位点）及裸引用直传同形腿；该拼写现状失败形态26枚探针无一钉过，实施前先钉现状探针、登记现状诊断作差分基线。
- **跨模块消费面**（§五跨模块裁决）：导出各形态默认值——带`= noop`默认的自由函数、带`= 他类限定静态方法按名引用`默认的构造器（补齐该子形态的消费方侧重绑覆盖；重绑语义同时由`= noop`与静态方法调用两腿钉住——`EncodeCallDesc`注释自证消费方按本模块路径重建）、带`= 他类限定静态字段`默认的构造器、带`= 他类限定静态方法调用()`默认的构造器——消费方import后即使实参全显式也须编译运行正常（扩充子集重建生效）；lambda默认的跨模块正形腿一，触发§五分级回退时改钉导出侧显式拒绝腿；方法宿主默认lambda两腿（§五跨模块边界）——同模块含实例捕获放行腿、导出含实例捕获显式拒绝腿。
- **文法臂**：bison冲突台账核对作实施计划显式步骤；语句形态`mws[0](1);`与表达式形态共用同臂。
- **stdlib**：`getOrDefault`单元测试＋e2e；预置源改动后重建拥有者目标并刷新build-tree stdlib磁盘拷贝再复跑语料（陈旧exe/陈旧拷贝两先例族纪律），`containers.n`不进包、无重打包腿。
- **Set（D-6）**：风险①②先行探针（预置包非native泛型类实例化、未用`Set`的程序产物不含其字节码）失败即回炉裁决；e2e钉方法面全表——`add`首插true重插false且`count`不变、`contains`/`remove`双态、`values()`保插入序、`copy()`独立性（改副本不改原件）、`toString()`格式、`int`与`string`两种元素类型各跑（钉判等语义随`Dict`键），`typeName(Set<int>)`裸名拼写与转型/判等一体腿（D-6拼写归类裁决）；未用`Set`的既有e2e语料产物字节对拍预期不变。
- **门禁**：收尾按影响映射Debug层；文法与resolver属编译器面，IDE树全量ctest随版本收口批执行（既有分层规则，不在此展开）。
