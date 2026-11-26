# NLang Roadmap

## 项目背景

NLang是一门面向嵌入与自动化场景的类型安全语言，配有字节码编译器和虚拟机，并作为探索AI友好语言特性的试验台演进。

所有的任务，在必要的时候可以改变语言、编译器和VM等设施，合理性优先，目前不需要向下兼容。改进语言、编译器和VM，是各阶段任务的目标之一。

> 各阶段的实施细节（commit号、opcode清单、陷阱记录）归档于git历史；本文件只保留阶段成果摘要。

---

## 已完成阶段

### 阶段0-5：语言核心 ✅
- **0基础框架**：AST/Visitor（X-macro自动生成）、flex/bison解析、VmBackend代码生成、VmExecutor、ncc/nvm/ndisasm工具链、.nmod模块系统
- **1基础特性**：int/float/string、算术/比较/逻辑、int↔float转换、if/while/do-while/for/break/continue、函数与递归、字符串拼接/比较/length()
- **2复合类型**：enum、switch/case、struct（值语义深拷贝、嵌套）
- **3类和对象**：class、new、this、引用语义、构造函数、单继承、virtual运行时分派、访问控制、null fail-fast
- **4数组**：固定/运行时大小数组、length()、嵌套数组、元素类型覆盖全部基元与引用
- **5 struct-class互嵌+ GC**：标记-清除、安全点触发、精确扫描（LocalDescriptor）

### 阶段6-8：抽象与持久化 ✅
- **6接口**：interface/implements、接口类型变量、按名虚分派、多接口
- **7调试支持**：字节码行号映射、调用栈回溯、NPE/异常自动打印
- **8序列化**：ByteStream/FileStream；struct/class字段、顶层object graph、多态、shared ref复用、cycle检测

### 阶段8e：泛型容器与表达式体系 ✅
- **8e-1/1.5**：隐式Object基类+ Equals/GetHashCode协议+基元装箱；`expr as T` 显式拆箱/向下转型
- **8e-3/4**：内置泛型 `List<T>` / `Dict<K,V>`（擦除式、合成类型声明、双签名、旁路存储表、per-method boxing plan）
- **8e-5/6**：`foreach`（索引式展开，零新opcode）；集合初始化器（bare `[...]` 与 `new Type{...}`）
- **8e-8**：二元表达式对称类型提升（`1+2.5 == 2.5+1`）
- **8e-9**：命名约定统一（类型PascalCase / 成员camelCase）；基元与Object.toString() 协议、隐式coercion；collection toString

### 阶段9：高级语言特性 ✅
- **9a** 增量赋值（`+=` 等）+ `assert` + `const` 局部
- **9b** 字符串插值（`"${identifier}"`）
- **9c** 默认参数+命名参数（重载评分+歧义检测；跨模块默认参数Option B）
- **9d/9d-2** 异常处理（try/catch/throw +内建Exception子类+字段暴露）+ finally（完整Java语义）+ `super()` 构造器链+裸字段implicit this.field
- **9e** out参数（OP_CallFuncOut/CallMethodDirectOut + outMask写回）
- **9f** native函数绑定（`native` 声明+ RegisterNative按名派发+ ncc/nvm崩溃报告器）
- **9系列稳定性修复**：GC根集根本修复（.nmod序列化func.locals，v1.5）、pResult累加器过期家族修复、frame布局越界（ASan扫描）、条件类型强制int、字符串转义补全、`>>` 拆分支持嵌套泛型、void函数、List/Dict下标语法糖

### 阶段10：IDE移植 ✅（2026-08-20）
- Qt5 nide：项目模型、编辑器管理、语法高亮、解决方案树、编译输出、对话框、MainWindow、部署自包含检查、端到端journey测试（Steps 0-10全部完成）

### 阶段11：标准库 ✅（2026-08-21）
- math（25函数+确定性PRNG）、io（print/readLine/readFile/writeFile）、fs（路径/目录/文件操作，谓词不抛、失败IOException）、string 12个内建方法（字节语义）；模块格式v1.7

### 阶段12：switch升级+ enum方法 ✅（2026-08-23）
- switch：逗号分隔多值case标签、类型化相等（int/float/string三分派）、判别值类型族门、重复标签编译期拒绝
- enum：Java式用户自定义方法（`enum E { A; int f() {...} }`，this=int32语义）
- 同轮根修：裸调用绑定方法编译期拒绝、数组接收者门（`Color[] a; a.rank()` 家族）

### 阶段13：函数/方法代理+类型别名 ✅（2026-08-25）
- **Step 0** `using Name = Type;` 类型别名（TU作用域+合并前展开预遍历）+ **Step 0.5** AST容器模型统一（out-of-list槽位收编+ DetachChild公共API）
- **Step 1** `Func<返回, 参数...>` 内建泛型函数类型+自由函数引用（MakeFunc/CallDelegate/Eq_func/Ne_func/Func_to_str；GC 8追踪位点全落；v1.8）
- **Step 2** 绑定方法引用（this捕获、状态跨调用保持）+虚/接口运行时派发（MakeVFunc按名句柄）+ out委托（CallDelegateOut移位写回）；绑定期空接收者守卫；native/enum/虚+out/默认参数等12类具名拒绝；三句柄形态跨模块往返
- 8个新opcode、RTK_Func=7三槽堆记录、`this==0 ⟺ 自由函数` 分派不变量；765 e2e

### 调试器ndb ✅（2026-09-07）
- `.nmod` v1.9：per-function sourceFile（跨文件断点寻址）+ B.1导入合并补拷locals（兼修既有GC根集洞）；D5语法修复（SnFunction产生式锚定Type——原 @2 NodeFlags位置TU恒null）
- VM：进程内 `IDebugHooks`（语句/throw检查点，D6勘误后三raise位点统一FireOnThrow）+ `IVmDebugView` 只读冻结视图（前端无关，DAP/nide可复用）；指令打印抽取共享 `Disassembler`（ndisasm golden逐字节对拍）；D7行标记去重（LocalDeclStmt双锚点）
- ndb：断点（file:LINE / LINE / funcName，同行多锚点全设）、步进s/n/f、bt、frame、info locals（隐藏名过滤）、p、l（SourceCache三级解析）、x（pc标记）、catch on|off；初停gdb start语义；EOF=q
- 836 e2e（含9个dbg_*）

### 数组类型属性化（数组重设计B）✅（2026-09-12）
- 属性化：数组值array-ness由表达式形状事后推断改为resolve期绑定stamp（`SnExpression::IsArrayValued`，五个表达式形状绑定尾写入）；8个门位+ `.length` 接收者（resolver/codegen两侧对称）改查属性；形状推断谓词家族（IsArrayValuedExpr/IsArrayTypedBase）删除——调用点归零
- 三处直修（同根：`EvalDataType()` 对数组字段返回元素类型）：字段kind改喂声明类型表达式（`int[]` 存RTK_Array——writeStruct静默腐蚀/类字段toString报错/GC欠追踪一家）；MarkPhase字段显式路由+撤运行时kind兜底+防御性RTK_Array元素追踪臂；writeStruct数组字段死抛错臂激活（具名拒绝）
- 边缘语义具名化：jagged `T[][]` 六声明位点resolve期拒绝、foreach非容器源拒绝（string源含）、`.length` 任意数组值接收者可解析（`li.get(0).length`/`lib.mk(3).length`）
- `.nmod` v1.10语义地板（字段kind语义变更烤在格式里，旧模块须重编译）；852 e2e / ctest 27+41

### 数组类型一等令牌（0.7.3表示层根治）✅（2026-09-25）
- 数组类型成为一等驻留令牌（`SnArrayTypeToken` + TU级驻留表，指针同一即类型同一）：声明侧与值侧共用同一令牌，`IsArrayValued` 由令牌派生；侧信道（`m_bArrayValued`/`GenericArrayFlags` 家族）整族退役，0.7.2位点级守卫族整删，数组转换裁决统一收敛GetCastInfo
- 语义收口：协变别名（`Base[] ba = da`）与 `enum[]`↔`int[]` 互通编译期拒；string强制转换全位置统一（string形参 / `io.print` / 元素槽）；string下标编译期拒；foreach数组值源翻正（源单次求值）；数值二元 / 关系比较 / 条件位 / null重载平手 / 容器方法值实参全部具名拒绝
- `.nmod` v1.12布局地板：递归类型描述符（真形参 / 返回 / 字段类型，嵌套数组与List/Dict，深度帽8）——跨模块stub真签名重建，跨模块调用点类型检查与同模块一致；973 e2e / ctest 46

### 统一库机制（0.8.0阶段1–4c）✅（2026-09-29）
- 标准库与第三方库并为一套机制：`stdlib/*.n` 是手写的权威声明，`nlang_<ns>.dll` 经宿主ABI提供native实现；单段 `import` 命中的库源被**完整内联**为库TU，限定调用编译为 `OP_CallFunc`，运行时按被调函数自身性质分派（字节码体 / `isNative` → DLL）
- 搜索路径五层统一（`-I`、`.nproj` `<ImportPaths>`、项目目录、`NLANG_PATH`、系统缺省），编译期发现 `.n` 与运行期加载DLL共用同一有序列表；nide叠加工具选项+项目属性两级配置
- 库的类型面可用（4b-2）：类型位置写 `ns.Type`，解析器在被内联的库单元内查找class/struct/enum/interface并绑定，继承、虚分派、enum方法与interface上行转换同项目类型等价；未import的命名空间直接诊断
- 内建标准库退役：硬编码签名表、math/io/fs三套intrinsic家族及其id块（70-94、110-114、120-127）全部删除，`OP_CallIntrinsic` 只保留接收者分派的内建方法（string 12个、流与容器协议）；ctest `no_builtin_stdlib` 守护不留回归路径

### 基本类型完备化（0.7.5）✅（2026-09-30）
- 12标量基元注册表（byte..ulong整型家族+ float/double + bool + char，RTK 10..19，单一PrimitiveTypes源）；字面量按值域分层、常量适配、混合算术最小容纳提升；有损隐式转换警告+ `ncc --no-warn`
- 严格bool：比较与谓词产bool，五处条件位置与 `&&`/`||`/`!` 操作数仅收bool（`equals` 保持int协议例外）
- double精度默认：无后缀小数与指数字面量 = double；math浮点族全double（floor/ceil/round→long）；数值指令改为带kind立即数的通用族（函数指针表分派，专用数值opcode整族退役）
- char与string桥接：`s[i]` 字节访问（废止0.7.3编译期拒）、码点迭代、charAt/charCount/toChar、`\uXXXX` 转义；码点层补齐Unicode支持
- 流64位原语（writeLong/readLong/writeDouble/readDouble）+全基元打印（io.print扩面+单一OP_Prim_to_str，double最短往返渲染）
- nide两级编译器选项（Tools→Options全局+工程属性覆写，首项警告抑制）；ndb局部变量类型感知渲染+ ndisasm标量kind短名
- `.nmod` v1.13语义地板（标量kind扩表）；1054 e2e（manifest 1034 + 20示例）/ ctest 50

### 交互输入补全 ✅（0.8.0，master线）
- `io.write`/`io.eprint` 补齐输出族；宿主API新增 `IHostIo::ReadInputLine` 读行原语
- ndb `stdin` 命令向被调试进程注入标准输入；nide运行输出页内嵌输入行，交互程序无需离开IDE

### 逐单元产物与加载期链接（0.8.0阶段5–6）✅（2026-10-03）
- 产物翻转：单文件编译产出 `.ncu` 单元映像、项目编译打包 `.npkg` 程序包（每源单元一成员＋入口记录）；产物不内联库代码，`.nmod` 退役
- 执行链统一走nloader→nlink：nloader沿搜索路径发现并装载引用闭包（版本与成员校验和校验，缺包诊断一次报清）；nlink纯内存合并（按限定名去重、导入占位槽解析、全操作数重映射）成唯一运行期模块——nvm、`ncc run`、ndb全部经此链（nide调用这些工具随之继承）
- 编译器内合并产码链（跨单元导入合并与字节码重映射）整链删除，加载期链接是唯一路径
- `.ncu` v2.0布局（导入槽＋逐单元模块路径）；合并线抬到v2.1（1.13/1.14语义变更并入2.x，旧1.x与合并前2.0映像一律拒绝）
- native动态库按顶层包段命名 `nlang_<package>.dll`；标准库以 `stdlib.npkg` 随工具链分发
- 手册双树围绕逐单元产物模型重写（命令行工具、模块与导入、调试章）

---

## 进行中

- **双线分工的历史预案（2026-11-16，docs/dev/dev_master_merge_design.md§七，未执行）**：原拟先合入dev侧的JSON库，再由master完成v0.13.9与v0.13.10；实际交付顺序相反，见下一条。该文档仅记录当时的裁决，不再作为当前排期依据。
- **双线合并执行（2026-11-23）**：上一条预案未及执行，两线继续分叉至合并基`d735f6c9`（2026-11-20）。master线此后交付v0.13.9（2026-11-20）与v0.13.10（2026-11-22）并开出v0.13.11的lambda审查修复批；dev线完成json成版（仍标0.13.11，NCU停在v2.22、宿主ABI升13）、门禁计时批Task 10-12（`tools/gate_time_*`族）与HTTP性能对拍工具。合并已执行：dev的136条提交随合并提交进入master，`[0.13.11]`段两侧条目融合为一个版本（json＋lambda审查修复批），NCU取master的v2.23（dev的json无格式面改动，直接兼容），宿主ABI取dev的13；手册格式章按dev侧重编号落位（json插为26章，ncu/npkg/abi顺移27/28/29），master侧旧编号ncu章的v2.23内容移植到新编号页。该合并版经审核与门禁收口于2026-11-24关闭（T1全腿＋手册五腿绿，含批内新鲜度前置检查上线前的判决打捞——各腿判决文件19/19 PASS）。旧合并设计文档docs/dev/dev_master_merge_design.md按历史文档保留，其未执行状态由本条记录。
- **双线二次合并执行（2026-11-26，发布v0.13.13）**：首次合并（上一条）后两线再次分叉——master线交付包产物的编译期消费与标准库十三包拆分F1（[0.13.12]，概要设计docs/dev/package_artifact_design.md）；dev线交付web框架实施面收口（[0.13.12]，tag v0.13.12已发布）与库分层重构（[0.13.12a]，tag v0.13.12a）。本次执行合并（dev `ea2c6f96`并入master），合并批次发布为v0.13.13，后续功能版本顺延（用户裁定2026-11-26）：
  - CHANGELOG：两侧同号[0.13.12]段融合为一个版本（F1包产物＋web框架＋lambda审查修复批，条目按面并集）；[0.13.11]取2026-11-23合并版（ABI 13条目保留master较新措辞）；[0.13.12a]取dev逐字；新增[0.13.13]条目记录本版合并发布与手册章顺移。
  - 版本顺延：0.13.13由本版（合并发布）占用，后续功能版本顺延一位——原v0.13.13/14/15的泛型完善（一）（二）（三）改列v0.13.14/15/16。
  - 手册：web插为库参考第27章，格式章ncu/npkg/abi顺移28/29/30（中英双树同步）；本文件以dev侧层级化形态为基底融合master独有条目。
  - 分层保全：nlang_core拆分（`nlang_vm`与`nlang_compiler`解耦，用户指示保全）原样进入master；分层守卫复核与T2收口门禁随后续发布批次执行。
- **0.12.x系列：内建声明真源化收尾＋用户定义泛型**（用户指示2026-10-28，已交付）：
  - v0.12.0：系列首版——数组/字符串声明面进预置包（承接0.11.x内建声明真源化两阶段；按class重实现的设想已于2026-11-01否决，见远期特性数组实现路线结论），用户定义泛型`class Foo<T>`完整落地（含误拼诊断文案改写：现"not a built-in generic type"误标声明模板臂，v0.12.1探针已钉该文案）。
  - v0.12.1：闭括号配对修复落地（嵌套泛型`Box<Box<int>>`解锁，探针已翻转）。
  - v0.12.2：数组/字符串声明面统一（预置面类String/Array声明方法面、resolver按声明校验、类型位拒绝门、签名表退役改绑定表取id）。
  - v0.12.3：并发类拆分（thread.n/concurrent.n两包分工，语言核心与基础容器恒可见的§12.4一句话规则）。
  - v0.12.4：const聚合（`const class`/`const struct`类型级不可变＋NCU v2.12标记）与Exception不可变＋cause链＋失败报告沿链输出。
  - v0.12.5：预置包拆分（语言核心进`lang.n`、基础容器进`containers.n`，builtins单体退役，`import builtins;`成为未知包，项目同名源文件即该包副本）。
  - v0.12.6：静态成员与静态局部（class/interface静态字段＋函数内静态局部＋`static const`编译期折叠/懒惰初始化、初始化异常可捕获重试、循环import编译期禁止；docs/dev/static_members_design.md）。
  - v0.12.7：数组/张量统一（单一类型`Array<T>`＋方括号静态秩标注与边界秩检查、O(1)视图/slice/广播、数组进Object、序列化数组内容＋调试器视图显示、fs二进制读取对，MNIST MLP验收0.897；docs/dev/array_tensor_design.md）。
  - v0.12.8：网络库（net包：TCP监听器/流、UDP、ICMP ping、DNS解析作业，ABI v11 IO停泊与工作池回调，docs/dev/net_library_design.md）。
  - v0.12.9：合并发布（0.12.8网络库并入主线、0.12.7打标后修复批收编、两轮对抗评审修复批）。
- **v0.13.0：值相等批＋手册已知限制清偿**（已交付2026-11-08；用户裁定2026-11-07：并入v0.13.9第①期深值比较＋泛型struct实参解除，其余期次留v0.13.9）：
  - struct `==`/`!=`按字段值比较（`OP_Eq_struct`/`OP_Ne_struct`，NCU v2.14，比较门拒绝排序/异类型/null）；容器元素与Dict键struct按深值、func按句柄内容三元组匹配（`StructValuesEqual`/`FuncRecordValuesEqual`）；用户模板接受struct类型实参（边界深拷，体内T→T搬运别名为已文档化角落）；func类型作Dict键的门删除。
  - const前缀顶层函数两拼写统一具名拒绝（TopItem文法诊断臂＋`CheckFunctionModifierContract`，冲突台账29+3→30+5）。
  - 手册语言页删6条陈旧条目（三条跨模块已由绿测试证伪＋List<struct>/List<func>恒等＋const函数）、VM页删2改1（IntList死引用改`int[]`原生存储）、比较/容器/序列化/字节码页同步值语义；CHANGELOG迁移注记（Dict按值键去重、struct==翻转、dedup写法可用）。
- **v0.13.1：运行时泛型类型参数信息**（已交付2026-11-10；用户指示2026-11-01，原排0.12.8，网络库占用后于2026-11-07改排）——接通实例化描述符双载体（对象头部＋调用点隐藏实参，宿主ABI v11→v12）、`typeName(类型表达式)`语言表达式（具体类型编译期折叠为字面量、类型参数`T`在共享方法体内读运行时描述符返回规范拼写string，用户裁定）、转型按描述符递归比较区分实例化、catch泛型类型编译期拒绝、`.ncu`模板表（无源码预编译包铸新实例化）；静态字段补偿模式随本版写入语言规格与用户手册（用户指示2026-11-01）。目标、缺口取证、非目标与验收见docs/dev/generic_runtime_typeinfo_design.md；设计此功能时应全面考虑基本的运行时类型信息与辅助设施的支持。
- **v0.13.1a：泛型完善**（已交付2026-11-12）——静态引用T全面化＋游离函数泛型支持，实现帧隐藏描述符槽与调用点隐藏实参。
- **v0.13.2：泛型递延打磨**（已交付2026-11-12）——显式实例化拼写按形状规则判定（C#式，无大小写检验不查名字表），拼写相撞的三类具名诊断（值头部给出加括号比较形式、函数头部说明调用不携带类型实参拼写、非泛型类型头部说明不接收类型实参）；裸`T`默认值拒绝消息两路径归一；类型参数上限255（约束通道为typeName/T判等序号字节的0xFF无序号标记哨兵）。拼写歧义的具名指向探针（identity规则）递延至v0.13.3。
- **v0.13.3：抽象函数和泛型接口**（已交付2026-11-13；ABI v2.17→v2.19）：
  - 类级抽象函数：无函数体的声明即抽象方法（无`abstract`关键字，抽象隐含虚）；含未实现义务的类为抽象类，实例化报错具名首个未满足义务；构造器/static/native拒绝抽象声明；抽象声明跨模块以纯签名记录传递，运行时对越界工件保留具名异常兜底。
  - 泛型接口：`interface Pair<T, U>`声明类型参数，实例签名用参数、静态方法带体对参数编译一次；实现以实例化拼写`class Both implements Pair<int, string>`逐签名检查，每个实例化是独立接口类型；静态经实例化拼写调用，跨模块同路径；接口常量不可取类型参数、T形参无默认值。
  - 拼写歧义具名指向探针落地（identity规则救援形状规则的拼写相撞）；数值宽度别名扩至全部数值基元（`int8`..`uint64`/`fp32`/`fp64`，诊断与`typeName`恒印规范名）；移除试验遗留保留字`elseif`/`state`；类型参数数组创建在创建点拒绝（与手册、设计D2口径对齐）。
  - 收官发现并修复单进程先编译后运行宿主的泛型缺陷（编译后端纯拼写编键、intern池收归运行时，ncc compile+run的泛型转型拒绝与序列化崩溃消除）。
- **v0.13.4：字符串处理增强/URL库**（已交付2026-11-08；docs/dev/http_library_design.md）：
  - String三方法（`indexOf(t,from)`/`toInt(radix)`/`equalsIgnoreCase`，VM内建intrinsic）与`io` Base64四编解码。
  - `system`包（`wallMs`/`monoMs`双时钟＋`DateTime` UTC日历类型，新native模块`nlang_system`）；`io.nowMs`迁`system.monoMs`。
  - `url`包（RFC 3986 `Url` parse/resolve/toString/queryParams/encodeQuery＋percent/form组件编解码）。
  - null退出string值域＋`string.EMPTY`常量（用户裁定；已知限制：跨native边界null句柄抛异常）。
- **v0.13.5：HTTP库**（已交付2026-11-09；docs/dev/http_library_design.md）：
  - 纯NLang的`http`包：消息模型`HttpRequest`/`HttpResponse`/`HttpHeaders`＋加固`HttpParser`、`HttpServer`每连接一虚拟线程＋400/408/413/417/431/500/503/505加固应答＋`Expect`/100-continue、`HttpClient`按origin连接池＋重定向＋连接/读取/整体三档独立超时＋默认头、requests式自由函数`get`/`post`/`put`/`del`/`request`、`HttpException`携响应`status`。
  - 流式正文：`HttpBodyReader`的`read`/`readAll`/`bodyText`/`close`/`drain`、有界`Channel<byte[]>`＝背压、写失败→关通道→`ChannelClosedException`＝取消。
  - `gzip`包（整块缓冲`compress`/`decompress`，新native模块`nlang_gzip`内置zlib）。
  - 示例`http_hello.n`/`http_conc_bench.n`/`http_sse.n`＋并发验收套件（百次压测）；手册新增第23/24章、格式章重编号为25/26/27。
- **v0.13.6：net/http库性能优化**：
  - 1）修复HTTP库实施期间发现的语言层面缺口（余留缺口的最新清单见远期特性「HTTP库实施期间的语言层面缺口」条）。
  - 2）性能门槛（用户裁定2026-11-10：与Node.js处于同一数量级，即同机同参数吞吐比值不超过10倍，单连接与高并发两行同时判定，未达门槛不得收版）本版收口时达标：主表单连接1814 RPS、高并发5907 RPS，与同窗Node.js的吞吐比值0.17与0.35，两行均在门槛内；高并发行的表观下降经三body档同探针复测（git `ee84a593`）判为整机窗口漂移而非代码回归，1 KiB跨版本落差属基线文档已钉死的「量子上限退场后正文成本显形」形态。判定口径见docs/dev/net_http_perf_design.md§一与§七，五方对拍（nginx、node、Go、Rust主流生态）数据与可重跑设施在docs/dev/http_perf_baseline.md与tools/http_perf/。
  - 3）`http.n`每响应的两处已定位成本已清偿：响应头与正文合并成一次发送（nginx与hyper都对小正文合并），`Date`头按秒缓存、不再每次重格式化（原实现含两次数组分配与13次串接）；配对A/B复测收益记在docs/dev/http_perf_baseline.md§5末与§6。
  - 4）`TcpStream.readLine`的缓冲归属已裁定并落地（D5）：缓冲归`TcpStream`自有的单个8192字节惰性接收窗口，`read`/`readLine`/`readExactly`先消费窗口、行长超过窗口的行跨补窗装配；`http.n`行读取器自带的`FILL_WINDOW`改为通过新增公开面`peekFilled(minBytes = 1, timeoutMs = 0)`与`consume(count)`读取和消费。逐字节解码串接已在修复批消除（同机同一次锁内对拍，64 KiB单行459 ms→243 ms），逐字节一次系统调用的形态随D5接收窗口消除（同输入按8 KiB块读的85 ms档达成）。
  - 5）`net.ping`的`timeoutMs = 0`（不设截止时刻）按D6处理：无截止的ping不再排进优先级1的原生工作池，不再推迟优先级0的主机名解析；完全走兴趣表停泊的根治形态随D8递延（Windows的`IcmpSendEcho2`句柄不是可轮询描述符，等平台侧等待形态定案），当前契约已在§21.4写明。
  - 6）net包的POSIX实现此前从未编译过（本机无Linux CI），本版已清偿编译期缺口——`net_icmp.cpp`include了只在Windows可用的`net_platform_internal.h`来读线程局部错误文本（改走跨平台访问器`NetTakeLastErrorOverride()`：Windows交出并清除该文本，POSIX无覆盖恒返空串）、`net_platform_posix.cpp`落后于`net_platform.h`的`NetResolve`/`NetIcmpEcho`参数与缺失的`NetResolveErrorText`定义；源码规模守卫同时暴露`net_platform_posix.cpp`超500行，按Windows的布局把ICMP条目拆成独立的`net_platform_icmp_posix.cpp`，两个POSIX翻译单元共用的套接字助手移入新的`net_platform_posix_internal.h`——并新增`tools/posix_net_build_check.sh`（Linux或WSL下编译该模块的Linux源文件清单并核对符号，判决行`GATE posix_net_build: PASS`）；这只证明编译与符号面，运行时仍未验；net族e2e的Linux实跑与该检查接入门禁，按用户裁定（2026-11-10）挪到nlang整体支持Linux时一并做，本版不做（docs/dev/net_http_perf_design.md裁定D8）。
  - 7）超过`maxConnections`的连接拒绝（`http.n`的`reject503`）按D7裁定：不再在accept线程上同步写完503再有序关闭，交给短工线程处理，连接风暴期间一批拒绝不再推迟其后的accept；在途拒绝已达64时超量连接直接关闭而无应答，客户端读到空响应，两种形态都写进手册`maxConnections`说明并有e2e覆盖后一种。
- **v0.13.7：位运算、正则表达式库和合并**（已交付2026-11-14，真合并提交14e824dd）：
  - 1）位运算完善：`& ^ | ~`与复合位赋值`&= |= ^=`，常量折叠随默认表达式语法，死令牌脚手架`++ -- ~=`与试验保留字`elseif state`一并移除。
  - 2）正则表达式库：RE2家族线性引擎（后向引用与零宽断言拒绝），面与语义以本机Go regexp对拍钉死。
  - 3）合并master和dev分支修改：统一opcode编号与NCU线格式v2.19地板、ASan运行库目录扫荡、七原生模块集（system/net/gzip入集）、手册格式章重建于26/27/28并补记v2.17–v2.19三档线格式增量、CHANGELOG同号[0.13.7]条目融合（裁决与逐面并集见docs/dev/v0137_merge_design.md）。
  - 4）并入tensor-simd线：数组算术运算符（降级tensor模块调用）、分层派发SIMD内核、全数组随机填充与无循环MNIST路径（工作版本0.14.0折叠入本版）。
- **v0.13.8：泛型术语统一＋跨模块功能完善**（已交付2026-11-14）：
  - 泛型术语统一（已随本版先期交付）：按docs/dev/generic_term_reference.md，recipe改称composition record、mint改用create/creation/instantiation词族，覆盖手册（中英成对）、设计文档、源码注释、标准库源与代码标识符，详见CHANGELOG[0.13.8]条目。
  - 范围核查修正（2026-11-15）：原列条1跨模块同参数数重载、条3枚举跨模块导入、条4导入struct实例字段访问三项为陈旧登记，经核已于0.10.14全部交付（known-limitations-priorities.md的2026-10-24条载明0.9.12三条残余边界全部消除，NCU 2.8→2.9，e2e回归钉imported_method_overloads/imported_struct_field_read/imported_enum_type在位），从开放范围移除，仅作回归保持，不重开工作。
  - 本版交付面·条5访问控制：跨模块游离函数和类的可见性完善，支持private/protected/public访问控制，类与函数均缺省public（D5.1裁定：可导入默认，封装以显式private/protected表达）；类型级与函数级前缀编译期强制，被拒成员对查找不可见——外来私有类型限定名报not-a-member、私有函数报标准未找到诊断；NCU线格式2.20函数记录尾与类型级访问字节、装载侧0或>3损坏拒绝。
  - 本版交付面·条8描述符限定身份：泛型描述符规范拼写的模板名改走包限定链接身份（根包类型即裸名），落实0.13.1概要设计D2的拼写规则（2026-11-14博客核对发现实现仍走v0.12.3裸名裁定：`DescriptorSpelling.cpp`的`DescriptorRowIndexFor`行名取`BaseName`、嵌套实参spelling字段同，与D2「限定拼写天然防跨包歧义」及泛型博客第二篇已按D2先行描述的措辞不符）；类记录行键维持裸名不动（登记键与拼写键分属两个键空间）；加载侧拼写解析与序列化往返兼容带点模板头；typeName包限定拼写、裸类型名解析自身单元优先、歧义诊断点名各持有者的包、跨包同名模板拼写互不相同、intern拼写键硬失败；验收补跨包同名模板e2e（可扩proj_generic_cross_unit）：不同包同名模板`typeName`互不相同、经`Object`跨包转型按限定拼写失败、静态字段补偿模式键区分。
  - 条6/7/9未随本版交付（2026-11-15收官探针实测：限定泛型拼写、导入函数接口类型返回值/导入用户泛型实例化降级、`class Sub : Box<int>`三种形态语法即拒绝），经2026-11-23分解由v0.13.14与v0.13.15承接，逐形状态在对应条目重新实测。
- **v0.13.9：值类型引用（&）**（用户指示2026-11-07；已交付2026-11-20）：
  - 设计裁决（docs/dev/value_type_reference_design.md，经用户/编译器专家/系统架构师三视角审核）：值类型（标量/enum/struct/string）保持值语义默认，以`&`显式引入引用（局部/形参/返回/foreach四位置；class等身份引用类型上`&`为等价记号）；统一表示（基准记录句柄＋cell偏移）＋记录稳定化（赋值/写入原位更新，堆分配收敛到新位置建立场景）；out参数退役、`&`形参全面取代；用户泛型struct实参解除；容器元素匹配改字段深值比较（配合项，销账已知限制第26/4项）。并发原位拷贝不保证单原子存储操作。
  - 交付（2026-11-20收口）：四期（①深值比较＋泛型解除→②记录稳定化→③&表示＋容器getter→④out退役，每期独立停靠，现行值语义测试全数不翻为护栏）在master线全部落地成版——①期已随v0.13.0于2026-11-08交付；④期把out的线格式（`OP_*Out`操作码、outMask通道、`PTDF_Out`位）一并清除，NCU升到v2.22。dev线不得把本版当成本线可开工的版本重做一遍。
  - 递延·测试表述（递延项并入既有规划版本裁定2026-11-12）：`stdlib_net_ping_stress`的lost-wakeup钉子仍以差分时间赌局表述（总量与单次时延双界），违反确定性测试裁定，待native侧确定性观测面（参照GC回收计数白盒钩子的形态）落地后改写断言。
  - 递延·期②终审（同裁定）：e2e分片（shard）执行在成为T1默认形态前须先做分片与非分片判决集相等的A/B验证，e2e_shard.py的并行docstring随该验证一并纠正。
  - 测试收口债（2026-11-16合并批实测登记、2026-11-20合并批核数，随本版清偿）：
    - (a) master带来的五份工具自测语料（`tools/e2e_shard_test.py`、`tools/test_impact_test.py`、`tools/tier_coverage_check_test.py`、`tools/gate_tier_test.py`、`tools/source_size_guard/check_source_size_test.py`）至今「有测试、无运行」——`tests/CMakeLists.txt`、`tools/gate_tiers.txt`、`tools/gate_tier.sh`与CONTRIBUTING都没有它们的注册位，三树ctest全绿也不覆盖它们，合并批只亲手跑了一次取结果，须挂进tier腿或ctest注册面。
    - (b) v0.13.8访问控制的D5.7验收e2e族只建了一半：跨模块顶层`private`三枚已在位（`xacc_fn_private`／`xacc_type_private`／`xacc_ok_public`，分别钉「导入private自由函数拒绝且诊断具名」「导入private类型限定名拒绝」「public照常」），仍欠两项——D5.7要求限定名与非限定名两条绑定路各自钉住，而`xacc_fn_private`把两种拼写写进同一表达式，只报得出第一条诊断；「protected同包可用」在包＝模块的现表示下与private行为重合、当场不可区分（手册§4.8.2已写明这条等价及其理由），登记为点分多模块包落地时的验收项而非本版可跑的腿。
    - D5.7的伪造产物项由单元测试承载（`tests/test_compiler/test_type_access_prefix.cpp`记前缀→FA_*字节，`tests/test_vm/test_nculoader.cpp`的`TestAccessBytesRoundTrip`记v2.20访问字节往返、`TestAccessByteOutOfRangeRejected`记FA_Default与高于FA_Public字节的装载拒绝），不另建e2e腿。
- **v0.13.10：Lambda函数/闭包**（已交付2026-11-22；概要设计docs/dev/lambda_closure_design.md，实施计划docs/dev/lambda_closure_plan.md）：
  - 语言面：`(params) -> body`lambda表达式经期望类型通道定型参数形态（零参/省略类型），`Capture`列表显式按值捕获、`&`引入按引用捕获，`const`/`static`/`this`捕获位形各有编译期强制；引用形返回打通`func<int&>`与`&`形参链；循环头按迭代逐次绑定（for与foreach，捕获语义配套）；闭包按closure identity比较、toString渲染`lambda`；零参lambda可作thread.spawn实参。
  - 实现面：display-class捕获合成（值捕获的display字段即存储、帧槽按位提升、GC标记byref捕获基句柄）；泛型上下文经合成实例化行重用调用点隐藏实参通道。
  - 格式面：NCU线格式v2.23——4槽函数记录＋配对实例化描述符＋lambda标志＋`OP_MakeFunc`的descIdx。
- **v0.13.11：JSON库**（dev线于2026-11-15完成JSON实现，随2026-11-23双线合并版交付，合并与收口经过见「双线合并执行」条；用户裁定2026-11-15：JSON先行成库，原v0.13.11「WEB框架」条目整体递延为v0.13.12）：
  - 纯NLang的`json`包：RFC 8259严格文法解析器（`json.parse`，违例报字节偏移）、DOM值模型（`JsonValue`抽象基类＋六节点类＋`JsonKind`）、类型化读取面＋`put`/`add`自动装箱构建面、紧凑/缩进两种序列化（对象键插入序稳定、数字原文保真、重复键后胜）、`\u`转义与孤立代理对U+FFFD、深度守卫先于VM千帧硬上限抛可捕获异常；`JsonException`直系挂`Exception`。
  - json包本身零新增原生、零编译器/VM改动；同版并入的lambda审查修复批、`as`加宽、宿主ABI升13与D-29名称解析修复另有编译器/VM面（D-29详情见v0.13.12条）。
  - 概要设计docs/dev/json_library_design.md。
- **v0.13.12：WEB框架与包产物F1**（用户裁定2026-11-15：web整体归本版交付；实施面收口见「交付状态」项）：
  - 范围：基于`http`包的web框架——路由/中间件/session/静态资源＋SSE helper＋urlencoded与multipart表单解析（400/413具名拒绝诊断）、`MultipartBody`构建器与上传文件名净化落盘；本项目只给HTTP分帧能力，不解释SSE语义。2026-11-16曾裁定排在lambda之前交付；实际lambda已随v0.13.10于2026-11-22交付，本版排于2026-11-23合并版v0.13.11之后，回调直接以闭包表达；lambda收编经用户裁定2026-11-23两段以合理性优先逐场景判定：回调体短、需要捕获、或省掉只为传回调而立的子类的场景就地用lambda，已落地语料一并复查（docs/dev/web_framework_design.md九.15）。
  - 交付状态：包级模块Tasks 1–7的实现已随2026-11-23合并版先行入树（`stdlib/web.n`1337行：`App`/`Router`/`RequestContext`/`Middleware`族/`Route`/`ValueBag`/`Session`族，五项`stdlib_web`e2e语料同在树）；三枚子模块（static/sse/form）与CMake递归glob已随dev线实施落地；CHANGELOG记录与手册web章已随实施成文。web框架实施面（实施计划Tasks 1–21）已于2026-11-25在dev线实施完成并经发布级门禁收口——除`e2e_runner_timing_gate`三枚因本机IPv4环回ICMP劣化经用户裁定豁免外全绿（2026-11-26用户裁定不解冻：豁免即常态口径，不再挂重启复绿账）。
  - 包形态（用户裁定2026-11-19）：`web`按点分多模块包落地，不再往单文件`stdlib/web.n`里堆。分层已由编译器那条硬约束推出并写回docs/dev/web_framework_design.md的D-28与§6.1：包级模块import自己的子模块即编译期硬拒的import循环，而组件必import包级模块，故包级模块公开面点名的类型只能留在包级模块——`stdlib/web.n`装`App`/`Router`/`RequestContext`/`Middleware`/会话族，三枚只单向命名核心的子模块`stdlib/web/static.n`、`stdlib/web/sse.n`、`stdlib/web/form.n`随Task 8、9、10落地（Task 6与7仍在包级模块内）；`CMakeLists.txt:267`的单层glob与`:291`的平铺拷贝已随第一枚子模块改成递归形态，`tools/test_families.txt`的`stdlib_web`族`src`规则同步补`stdlib/web/*.n`。`http`包本版不拆，见远期特性「标准库大文件改多模块包」。
  - 随本版清偿·HTTP库收尾两项：服务器优雅关闭（连接登记表＋排空/强关，含`HttpServer.close()`后重新`start()`/`serve()`不复位关闭标志的复位项）；net`TcpStream.read`区分超时与对端EOF。
  - 随本版清偿·测试收口债（2026-11-17登记）：`tests/test_nide/test_mainwindow.cpp:2323`的`QVERIFY(restart.elapsed() < 1000)`按墙钟判定终端重启走的是快路径（见证轮询应在一个20 ms片内落地，只有3秒兜底路径才会越线），与确定性测试设计裁定（测试断言不得依赖时间，超时帽只当挂死护栏）冲突；改法是钉住「走快路径」的可观测状态而不是耗时。本项不在类型词汇表修复批内改写——该批的IDE树门禁结果正是在这条断言上取的绿，改写会让已有结果作废。
  - 名称解析修复（用户裁定2026-11-19：现有VM或编译器不合理处先改正、而不是迁就现有实现，不需要向下兼容；docs/dev/web_framework_design.md的D-29）：已随2026-11-23合并版提前落地并记入该版CHANGELOG的Fixed——`import p.*;`对库包同样并入`p`及其全部子模块（`Wildcard import cannot target library package`诊断随之移除），限定名的成员解析按最长模块路径优先、其次类型统一到任意深度与全部成员种类；手册§3.3「库包是单个单元」前提已改写，§3.6、§4.8.8与docs/dev/name_resolution_design.md的带日期修订已一并落好。裸类名写法继续有效，裁决40的「本版不做」据此作废。合并评审（2026-11-23）补齐了裁决里m12平局的实现缺口：同名类型对同名子模块的优先此前的类型探针只查最长已知前缀，深度平局时子模块反超，与裁决、代码注释和手册句三方相悖；现由共享的捕获谓词在每个已知前缀上取最浅类型锚（调用、函数值、类型值、类型引用与类型声明五个路由统一），类型声明位对穿类型的限定链给出「a type name cannot be qualified through it」新诊断。
  - 随本版清偿·诊断折叠缺口（2026-11-24修复落地）：从未被加载的第三方包（不在符号索引内）伸入类内部的限定链，其「is not imported」诊断此前按整链点名——主建议`import tp.sub.Fn;`在`Fn`是类时不可用，括号里的通配备选可用；修复后库模块可知性＝符号索引已注册∨库目录按点分名提供源码（`BuildEnvironment::HasLibrarySource`，与导入发现共用同一映射，无需目录枚举），未导入的第三方子模块同样折叠到最长已知前缀（`import tp;`＋`tp.sub.Fn.f()`报`Module 'tp.sub' is not imported. Add 'import tp.sub;' (or 'import tp.*;')`），惰性内联的包名由匹配根派生（`SymbolIndex::PackageUnderRoot`）。
  - 随本版交付·包产物的编译期消费与标准库按领域拆包F1（用户裁定2026-11-24：归本版不单列；概要设计docs/dev/package_artifact_design.md，四轮评审干净退出）：带限定符的第三方包不再只能是源码形态——编译期导入发现增产物臂（.npkg成员表为导入名解析的真源，`.ncu`直文件与包内成员两子臂同进发现相位；发现/门/装载三阶段相位裁定，外部条目发现期定型、RegisterUnits后物化；桩闭包不动点补传递依赖与跨镜像循环检测，兑现手册§3.9.3承诺；源码优先、产物补缺的双形态加载序），通配并集与D-29诊断折叠各增产物臂；容器格式1.0→1.1增包内native成员（运行期开包抽取到%LOCALAPPDATA%\nlang\pkg-cache，包路径哈希＋成员校验和为缓存键）；stdlib.npkg按领域拆为concurrent/io/math/fs/tensor/net/http/url/system/gzip/regex/json/web十三包（lang与containers系prelude嵌入不产物化；stdlib自身DLL迁移随Linux部署形态定案一并评估——D5递延锚；拆包编辑面在CMake打包链与工具脚本，src/与include/零代码字面量）。web包成员清单已随web线收口定案，F1拆包已随之落地（F1拆分与web线Tasks 8-10同CMake动作，顺序协调避免踩踏）。
- **v0.13.12a：库分层重构（nlang_core拆分）**（已交付2026-11-25，dev线）：
  - 拆分面：新增内部静态库`nlang_core`承载编译产物的共同词汇——模块字节格式、字节码拼写、反汇编器、加载器与链接器；字节码后端从`nlang_vm`迁入`nlang_compiler`（`src/compiler/codegen`）；共同词汇的公开头文件改到`include/nlang/core`之下。
  - 成果：斩断原先`nlang_vm ↔ nlang_compiler`的链接期环——编译器只建在`nlang_core`上，虚拟机回归纯执行。无用户可见面变化：语言、标准库、诊断文案、退出码与NCU字节格式逐字节不变（拆分前后构建的全部stdlib与示例产物字节对拍验证）。
  - 概要设计docs/dev/library_layering_design.md，实施计划docs/dev/library_layering_plan.md；分层守卫`tools/layering_guard.py`随本版入树。
- **v0.13.14：泛型完善（一）——跨模块类型签名与限定泛型拼写**（2026-11-23自原「v0.13.13+泛型完善」合并条目分解，0.13.8收官递延三项按规格依赖序6→7→9分配，本版承载6与7；原条目引用的docs/dev/generic_todo.md经git史核验自创建即为空文件，任务真源即路线图条目本身，空文件与引用随本次分解删除）：
  - 6）跨模块接口与泛型类型签名完整化（0.13.3收官实测登记）：导入函数的接口类型返回值重建为int占位符（产物无线格式接口表）；导入的用户泛型类实例化返回值降级为裸擦除名；导入类桩丢失implements边——三处同根，修复需名字承载的TypeDesc叶（或接口线格式表）＋ABI提升＋桩合成＋implements线清单；配套伪造产物wire尾测试装置（0.13.3审查递延项，随6）落地时一并建设；2026-11-24 spec裁定docs/dev/generic_signature_design.md D5取测试侧字节手术形态——真实编译产物原位改字节（先行例PatchMinorVersion/test_ref_ops/test_func_records），不建生产侧可注入旗标，抽象方法守卫族已由0.13.9跨单元成员谓词统一（`4db23954`，IsRegisteredMember，测试在tests/test_vm/test_perunit.cpp）交付取代、不再重建）。
  - 7）限定泛型拼写`lib.Pipe<int>`解析（0.13.3登记：QualifiedType无泛型后缀，泛型类同样受影响，与6）的跨模块泛型消费互补——限定拼写是跨模块泛型消费的书写形态，两题同版收口，共享一次ABI提升与wire尾测试装置）。
  - 2026-11-23重新实测钉死（探针语料temp/g13_probe/，sealed布局防源内联混淆）：接口三腿已由0.13.9接口表（D14）修复——消费者类implements导入接口、导入接口形参收本地实参、导入类桩上的接口分派、接口返回值全部通过；活缺陷收敛为三处——公开泛型类令整个.ncu导入中毒（TypeDesc无用户泛型实例化编码：生产侧把`Box<int>`写成RTK_Class＋模板行号，读取侧合成裸类名撞裸用守卫）、实例化签名降级（返回值成void/int32占位）、限定名＋类型实参在声明位与调用位都是文法级缺失；内部（非public）泛型类不中毒，导入泛型函数经推断实参可用。
  - 2026-11-24扩容（用户指示：跨模块lambda等不合理限制一并解决；**所有新增限制的取舍原则＝主流语言支持程度**，C#/Java/TypeScript为主参照系）：lambda/函数值族经探针矩阵钉死（temp/g13m_probe/FINDINGS.md，含单模块对照归因）——L1）lambda实参进泛型函数`func<T,T>`形参的调用点推断链（单模块与跨模块同败于推断/转换层：期望类型不进lambda、T不从后续实参绑定；跨模块侧无独立缺陷，导入泛型函数经推断实参可用已由pgenfn钉死），主流三大家全支持（C#泛型方法收lambda实参、Java同构、TypeScript同构），随6）的实例化描述符通道一并收口；C）func类型类成员令成员解析失败（字段为func类型或方法形参为func类型都令成员不可解析，且连坐同类无关正常方法——最小化M1/M2/M3；单模块与跨模块同败，非跨模块特有；游离函数func形参与局部赋值健康），主流普遍支持（C#委托字段/委托形参方法、Java函数式接口字段）；G）类型名上静态方法作函数值解析不到（resolver只查字段不查静态方法，G/G0对照钉死），C#方法组转换`Util.Thrice`同形；L0）lambda自带`<T>`形参表的显式拼写仅TypeScript一家支持（C#/Java/Kotlin/Swift/Rust皆无，泛型性一律经泛型函数与推断承载），按主流支持程度原则不引入该拼写，其使用需求由L1）的推断链承接。
- **v0.13.15：泛型完善（二）——继承实例化作基类与struct入Object**（2026-11-23分解；本版承载依赖序第9项与struct装箱缺口，两项同处「定义侧参数与值如何落到对象描述符位」这一语义层，各自的裁决一并收口）：
  - 9）类继承泛型实例化作基类：`class Sub : Box<int>`与模板继承实例化`class Wrap<T> : Box<T>`两种形态。2026-11-15用户裁定：2026-11-13远期登记的“无合理语义——模板的抽象成员无法由对实例化的继承补全，规格认可的死端”定性推翻，合理性优先，不支持`class D : Box<int>`本身不合理；原远期条目并入本项，不再单列（2026-11-14探针钉死：文法`ClassInheritOpt: ':' TypeName`无类型实参臂，语法即拒绝）。引入时须同时裁决“定义模板参数→对象描述符实参位”的映射规则（0.13.1概要设计D1遗留裁决，参数错序与具体实参会破坏按位读取）；0.13.1c的implements边替换（`7fac1dcc`）是现成先例。文法注意：`TypeName`与`as y < z`比较拼写共用LALR归约，泛型臂不能直接加在`TypeName`（nlang.y implements位注释载明该约束），须按`OT_TypeLt`扫描标志形状或专用产生式解决；验收e2e钉死两类继承形态与描述符实参位映射。
  - struct进`Object`位的缺口（2026-11-19实测登记）：struct今天在三个入口（初始化、赋值、实参）一律拒绝——实测语料`tests/e2e/boxed_arg_struct_reject.n`钉住实参入口，另两个入口走同一谓词；v0.13.12的编译器修复只补齐已装箱种类（标量/string/enum）在实参入口的缺口（手册`docs/user_manual/{en,zh}/specification/04-types/object.md`§4.11.3早已写明三个入口，缺的是实现），不改struct。struct要进`Object`得先定「装箱副本还是记录引用」，该裁决属v0.13.9的`&`表示（docs/dev/value_type_reference_design.md③期），v0.13.9的`&`表示现已交付；struct装箱的「装箱副本还是记录引用」裁决随本版实施。
- **v0.13.16：泛型完善（三）——描述符实参种类模型v2**（2026-11-23分解；跨层影响面独立成版、排在6/7/9之后：种类模型扩展不改变既有种类的语义，先行的6/7/9验收e2e在v2落地后仍须全绿，作为不改行为的回归护栏）——清偿0.13.8收官审查递延的「描述符实参种类模型v2」（驱动用例：转型`x as T`的T为接口类型实参时永不匹配）：实参种类引入RTK_Interface/RTK_Func＋implements遍历＋数组元素种类配对（RegisterArrayTypes通道）；elemKind牵动数组布局分类消费方（ArrayHeaderCells族），跨层影响面需独立评估，实施前须独立概要设计评审。

## 0.14.x：张量/设备后端线（tensor-simd分支，用户裁定2026-11-17按工作量拆分排期）

- **v0.14.0：张量设备后端**（已交付，tensor-simd分支）——可插拔设备后端（ABI v13：newArrayResident/setTensorDeviceHooks，VM永不链接厂商运行时）＋CUDA插件真跑（mnist 0.895）＋驻留模型＋后端选择。手册§18.7。
- **v0.14.1：设备端异步执行＋差异化同步**（已交付，tensor-simd分支，提交a04a1cca）——不产生cell级异常的launcher异步化（去每op同步，单流stream ordering保证池块复用安全），四个错误能力家族（int二元/int二元标量/gather/oneHot）保留每op自我排空，异常在op当场抛且文案与宿主逐字同；scratch改池化；顺带修复rdiv三处崩溃/假阳性、oneHot设备崩溃、空数组恒等式、符号pin漂移。mnist设备腿10.7s→3.0s（3.6×），宿主腿零回归。手册§18.7"执行模型"。
- **v0.14.2：索引表面完成**（已交付，tensor-simd分支）——数值数组逐元素比较算子产出int 0/1掩码（数值数组的`==`/`!=`从恒等比较改判逐元素，非数值种类保留恒等拼写）、`tensor.where`、`tensor.scatter`与花式索引赋值（`a[a > 0] = 0`免逐元素循环）；int标量操作数改精确int64承载，修复2^53以上静默损坏；CPU＋CUDA双腿parity。
- **v0.14.3：设备路径补齐**（已交付，tensor-simd分支）——`tensor.gather`以take-per-position内核落地全部元素种类与全部轴（界校验收在宿主侧、跨步视图按普通操作数处理）、整型族`clamp`、逐轴reduce/argmax族（含中间轴）上设备并删除f32专用轴内核；手册§18.7设备已知限制收缩为concat轴限制与延迟故障归因两条。
- **v0.14.4：可失败hook契约＋完全异步错误模型**（已交付，tensor-simd分支，分支内ABI 13→14）——`TensorDeviceStatus`厂商中立错误记录进入全部钩子契约（厂商错误码不进共享头，mirror后端同契约验证中立性）；粘性故障寄存器在每个观测点先行排水为数据，清偿"故障后仅打印"吞错窗口与设备OOM缺口。合并收口时ABI地板按本节末对账注记重排。
- **v0.14.5：流感知内存与计算/传输重叠**（已交付，tensor-simd分支）——手写设备池退役为`cudaMallocAsync`/`cudaFreeAsync`流感知分配（释放阈值令内存跨同步驻留，零字节病理随池退役）；≥64 KiB大块传输走staging流与计算流已入队内核重叠，事件握手保序；元素级传输与观测点契约不变。
- **v0.14.6：稳定排序对**（已交付，tensor-simd分支）——`tensor.sort`/`tensor.argsort`按轴行稳定升序（numpy语义：NaN排线尾、序号先到先得）；CPU逐行`std::stable_sort`、设备每行一线程插入排序、超4096单元的行回落宿主遍历；四种数值种类双腿齐备。
- **v0.14.7：索引与scatter完成面**（已交付，tensor-simd分支）——`takeAlongAxis`（argsort的消费者一等化，任意秩整行排序）、`unique`/`uniqueCounts`、`scatterAdd`/`scatterAddInto`（重复索引累加求和、设备走atomics——梯度拼写）；越界一律具名异常不wrap。
- **v0.14.8：切片赋值**（已交付，tensor-simd分支）——切片面的写半：`tensor.assignInto`/`assignIntoS`经视图把写入路由到owner记录；`m[0:2, 1:3] = v`降低为切片视图上的`assignInto`，读路径的形状/跨步/可写性为唯一真源；广播轴视图只读、赋值具名拒绝；设备核函数每视图单元一线程、标量支路携参免staging。
- **v0.14.9：流编排**（已交付，tensor-simd分支）——`tensor.streamCreate`/`streamDestroy`/`streamPush`/`streamPop`/`streamJoin`作用域多流（嵌套如花括号、join序化交接使主流其后看见被join流的全部写入），push域内`uniform`取不相交的全局序列私有窗口、`tensor.seed`令全程序可重放；cpu/mirror后端全no-op保持中立；`examples/mc_streams`四流蒙特卡洛确定性构造；修复逐轴行走地落跨步指针悬垂已释放临时（静默错误类，地落记录禁拷贝升级为编译期拒绝）。
- **多GPU**：维持硬件触发（本机单GPU不可真跑验收，以多GPU验收机到位为启动条件）；届时residency从布尔→设备索引、per-device上下文/流/池、跨设备传输语义、`tensor.setDevice`。
- 编号与ABI说明（三线roadmap合并时补记2026-11-25）：本线早期以工作版本0.14.0编号的张量SIMD批已折叠入主线v0.13.7交付（见进行中v0.13.7条第4项「工作版本0.14.0折叠入本版」），本线条目保留分支自己的0.14.x编号，两线合并收口时按主线版本地板统一重排；本线v0.14.0记的宿主ABI v13与2026-11-23双线合并版取的宿主ABI 13是两线各自独立递增得到的同值，合并时须按合并后真源重排ABI地板（v0.14.4已实际录用的v14随之顺延）。

## 远期特性

- enum方法引用；多播委托；Func协变/逆变
- 底层实现的内存管理全面RAII化：智能指针替换文法/构建器中的裸 `new` 与自定义 `UniquePtr` 容器（EnNew/EnDelete宏已于2026-08-23移除）
- 基于LLVM的本地字节码生成编译器
- 包管理器：模块依赖管理（阶段11延后项）
- 调试信息完善：编译期记录每个函数的源文件名（编译产物内字段，或随包产出编译附加信息文件），让回溯与断点显示真实来源——当前回溯帧以入口单元文件名搭配各函数行号，标准库帧会错位
- 数组特性延后项（0.12.7裁定登记，见docs/dev/array_tensor_design.md D14）：负标量下标（`a[-1]`＝len+neg换算，已裁定支持，牵连List/string下标一致性故实现靠后）；max/min沿轴归约（`ReduceAxisKernel`模板已随sum(axis)落地，有消费者时按需开启）；批量matmul（前导维广播语义）；slice表达式作赋值目标（视图左值机制）
- HTTP库实施期间的语言层面缺口（2026-11-09实测登记）：文件级变量不能与`main()`并存（现报"top-level statements cannot be combined with an explicit main() function"，共享计数器只能塞进只装静态字段的持有类）；类类型形参的实参不能写`f(null)`（须先声明一个值为null的局部变量再传）；同类`static const`不能用于实例字段初始化器。三者都是普通程序员会期待的行为，按合理性优先排期，不记入手册「已知限制」
- WEB框架实施期间的语言层面缺口（2026-11-19实测登记）：五条同属「声明处的作用域没接到默认值或实参的预期类型」这一族，框架侧各有不改语义的改写形制，web框架实施未被阻挡，按合理性优先排期，不记入手册「已知限制」。五缺口的探针坐标与修法都在docs/dev/web_framework_design.md的九.18。
  - 1）带默认值的`func`形参上下文拒绝函数引用（探针F3实测`rc=1`；lambda字面量作默认表达式同样被拒，探针`temp/lam_probe/p5_default_expr.out`，2026-11-23——声明侧不成立则整格不可用；修法是期望函数的解析延后到实参形态已知，或在期望类型解析里带上形参的声明类型；九.18第1条）。
  - 2）实例构造调用的实参位置不给函数引用预期类型：`FuncMiddleware(handler)`一类`Type(args)`写法把自由函数引用与绑定方法引用一起拒掉，而同一枚引用在普通调用实参、字段赋值与带类型的局部声明三处都能绑定（探针`temp/t6_probe3.n`），lambda字面量直接作构造实参同样被拒、先落带类型局部再交构造调用的绕行对lambda实测有效（探针`temp/lam_probe/p3_ctor_arg.out`、`p7_ctor_via_local.out`，2026-11-23）；与1）同属期望类型的传播缺口，一条修法覆盖两者，框架侧先把引用落进同类型局部变量再交构造调用（`App`构造`m_routeLink`即用此形制；九.18第2条）。
  - 3）容器元素不能就地调用：`mws[0](ctx)`一类一律语法错，必须先取进同类型局部变量（探针W1/W2/W4/W6/X6b），影响所有「把函数引用装进容器」的写法，不止本框架（九.18第3条）。
  - 4）`Dict`缺`getOrDefault`而`get`缺键抛异常（探针P4，配套`Set.contains`的风格一致性一并核；九.18第4条）。
  - 5）构造器形参的默认值取不到任何命名常量：本类静态常量的裸拼写与限定拼写、public静态常量、另一个类的静态常量四种拼写一起编译失败，且`Knob.K`这类根本不含`this`的写法也报同一句`cannot reference 'this' …`，而模块作用域只有函数与类型声明、没有常量声明可退（探针`temp/t7_ctor_default/`，闸门`src/compiler/builder/StatementResolverDeclDefaults.cpp:40-115`）；修法是默认值按声明所属类型的作用域求值、只拒真正需要`this`的实例成员访问，并把那句诊断按实际拼写分开措辞；`MemorySessionStore`的三档构造器因此按重载交付、不给形参默认值（docs/dev/web_framework_design.md的6.6；九.18第5条）。
- WEB框架功能扩张项（2026-11-23登记）：路由级中间件与路由分组（要改中间件链的构造期即改分派语义），落点与设计要点见docs/dev/web_framework_design.md九.5；正文其余本版不做特性以该文档第九章为准，不逐条展开（条目粒度与正文异构，双处登记会漂移）。
- 标准库大文件改多模块包（2026-11-19实测登记）：点分形态今天在源布局上已经可用——一枚包可以同时含包级模块与点分子模块（探针：一份`.nproj`列`main.n`＋`k.n`＋`k/w.n`，打包后运行`rc=0`，`k.n`内`import k.w;`即可达子模块；`import k.*;`把整棵子树并入，未导入时的诊断原文就是`Module 'k.w' is not imported. Add 'import k.w;' (or 'import k.*;')`）。拆一个既有大文件要付三笔代价：
  - 1）移出包级模块的公开类，限定拼写改成`包.子模块.类`，包级`import 包;`不再覆盖它们，用户代码与既有语料的import行都得重排（`docs/dev/stdlib_public_surface.md`、手册页与install-layout同步）。
  - 2）兄弟模块间的循环import编译期硬拒（探针原文`Error: import cycle detected: k -> k.w -> k`），拆分必须先给出无环分层，互相引用的类型只能落在同一层。
  - 3）预置包构建按`stdlib/*.n`单层glob并把文件名平铺进`.nproj`与build-tree拷贝（`CMakeLists.txt:267`与`:291`），嵌套源要改`GLOB_RECURSIVE`＋相对路径＋递归拷贝（`install(DIRECTORY stdlib/ …)`本就递归）——这笔已随web子模块落地时改掉（见v0.13.12条），后续拆分不再付费。
  - 第一个拆分对象是`stdlib/http.n`（2434行，`HttpConn`约697行与`HttpParser`约453行装的是线格式与加固规则），排在web线收口之后独立成版处理；`web`包已按点分形态落点（用户裁定2026-11-19：包级模块随2026-11-23合并版入树、三枚子模块已随dev线实施落地），分层约束见v0.13.12条与docs/dev/web_framework_design.md的D-28。
- 字符串维持特化存储＋装箱进`Object`（2026-11-01设计结论，取代原「基于class的字符串实现」）：字符串存放在独立存储中，句柄与堆索引共用一个空间因而冲突，装箱是唯一不动物理布局的一致性方案（Go的`any`对字符串同样是装箱）；免装箱的替代路线（并入对象堆、带标签引用）成本远超收益，如要评估须单独立项
- LSP支持：VS Code / JetBrains协议（用户指示2026-08-21：最后实施）
- Linux/macOS打包发布：Windows zip + NSIS（CPack）已落地（2026-08-26）；Linux暂缓，待源码跨平台移植修复后以CI构建TGZ/DEB
- 安装包组件化（tools-only / IDE-only）与捆绑VC++运行库（/MT或vc_redist）
- 文档子系统延后项：①片段审计扩展到specification/vm-architecture的**运行**审计（约68个bare围栏块的**标签分类**已由2026-08-30导航细化+语法高亮轮吸收；运行审计仍延后）；②站点语言政策决策（含用户2026-08-30指示的中英双站方向——机制选型mkdocs-static-i18n vs每locale独立build，后者对file:// 离线更稳）

---

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P4 | LSP支持 | 最后实施（用户指示2026-08-21） |

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/user_manual/{zh,en}/specification/ | NLang语言规范（语言、标准库、二进制格式、参考页），带§ 条款编号，中英双树各70页 |
| docs/user_manual/{zh,en}/vm-architecture/ | VM架构设计（编译管线、堆布局、GC算法、指令集、库机制），一章一页16篇，中英双树 |

