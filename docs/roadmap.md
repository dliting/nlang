# NLang Roadmap

## 项目背景

NLang 是一门面向嵌入与自动化场景的类型安全语言，配有字节码编译器和虚拟机，并作为探索 AI 友好语言特性的试验台演进。

> 各阶段的实施细节（commit 号、opcode 清单、陷阱记录）归档于 git 历史；本文件只保留阶段成果摘要。

---

## 已完成阶段

### 阶段 0-5：语言核心 ✅
- **0 基础框架**：AST/Visitor（X-macro 自动生成）、flex/bison 解析、VmBackend 代码生成、VmExecutor、ncc/nvm/ndisasm 工具链、.nmod 模块系统
- **1 基础特性**：int/float/string、算术/比较/逻辑、int↔float 转换、if/while/do-while/for/break/continue、函数与递归、字符串拼接/比较/length()
- **2 复合类型**：enum、switch/case、struct（值语义深拷贝、嵌套）
- **3 类和对象**：class、new、this、引用语义、构造函数、单继承、virtual 运行时分派、访问控制、null fail-fast
- **4 数组**：固定/运行时大小数组、length()、嵌套数组、元素类型覆盖全部基元与引用
- **5 struct-class 互嵌 + GC**：标记-清除、安全点触发、精确扫描（LocalDescriptor）

### 阶段 6-8：抽象与持久化 ✅
- **6 接口**：interface/implements、接口类型变量、按名虚分派、多接口
- **7 调试支持**：字节码行号映射、调用栈回溯、NPE/异常自动打印
- **8 序列化**：ByteStream/FileStream；struct/class 字段、顶层 object graph、多态、shared ref 复用、cycle 检测

### 阶段 8e：泛型容器与表达式体系 ✅
- **8e-1/1.5**：隐式 Object 基类 + Equals/GetHashCode 协议 + 基元装箱；`expr as T` 显式拆箱/向下转型
- **8e-3/4**：内置泛型 `List<T>` / `Dict<K,V>`（擦除式、合成类型声明、双签名、旁路存储表、per-method boxing plan）
- **8e-5/6**：`foreach`（索引式展开，零新 opcode）；集合初始化器（bare `[...]` 与 `new Type{...}`）
- **8e-8**：二元表达式对称类型提升（`1+2.5 == 2.5+1`）
- **8e-9**：命名约定统一（类型 PascalCase / 成员 camelCase）；基元与 Object.toString() 协议、隐式 coercion；collection toString

### 阶段 9：高级语言特性 ✅
- **9a** 增量赋值（`+=` 等）+ `assert` + `const` 局部
- **9b** 字符串插值（`"${identifier}"`）
- **9c** 默认参数 + 命名参数（重载评分 + 歧义检测；跨模块默认参数 Option B）
- **9d/9d-2** 异常处理（try/catch/throw + 内建 Exception 子类 + 字段暴露）+ finally（完整 Java 语义）+ `super()` 构造器链 + 裸字段 implicit this.field
- **9e** out 参数（OP_CallFuncOut/CallMethodDirectOut + outMask 写回）
- **9f** native 函数绑定（`native` 声明 + RegisterNative 按名派发 + ncc/nvm 崩溃报告器）
- **9 系列稳定性修复**：GC 根集根本修复（.nmod 序列化 func.locals，v1.5）、pResult 累加器过期家族修复、frame 布局越界（ASan 扫描）、条件类型强制 int、字符串转义补全、`>>` 拆分支持嵌套泛型、void 函数、List/Dict 下标语法糖

### 阶段 10：IDE 移植 ✅（2026-08-20）
- Qt5 nide：项目模型、编辑器管理、语法高亮、解决方案树、编译输出、对话框、MainWindow、部署自包含检查、端到端 journey 测试（Steps 0-10 全部完成）

### 阶段 11：标准库 ✅（2026-08-21）
- math（25 函数 + 确定性 PRNG）、io（print/readLine/readFile/writeFile）、fs（路径/目录/文件操作，谓词不抛、失败 IOException）、string 12 个内建方法（字节语义）；模块格式 v1.7

### 阶段 12：switch 升级 + enum 方法 ✅（2026-08-23）
- switch：逗号分隔多值 case 标签、类型化相等（int/float/string 三分派）、判别值类型族门、重复标签编译期拒绝
- enum：Java 式用户自定义方法（`enum E { A; int f() {...} }`，this=int32 语义）
- 同轮根修：裸调用绑定方法编译期拒绝、数组接收者门（`Color[] a; a.rank()` 家族）

### 阶段 13：函数/方法代理 + 类型别名 ✅（2026-08-25）
- **Step 0** `using Name = Type;` 类型别名（TU 作用域 + 合并前展开预遍历）+ **Step 0.5** AST 容器模型统一（out-of-list 槽位收编 + DetachChild 公共 API）
- **Step 1** `Func<返回, 参数...>` 内建泛型函数类型 + 自由函数引用（MakeFunc/CallDelegate/Eq_func/Ne_func/Func_to_str；GC 8 追踪位点全落；v1.8）
- **Step 2** 绑定方法引用（this 捕获、状态跨调用保持）+ 虚/接口运行时派发（MakeVFunc 按名句柄）+ out 委托（CallDelegateOut 移位写回）；绑定期空接收者守卫；native/enum/虚+out/默认参数等 12 类具名拒绝；三句柄形态跨模块往返
- 8 个新 opcode、RTK_Func=7 三槽堆记录、`this==0 ⟺ 自由函数` 分派不变量；765 e2e

### 调试器 ndb ✅（2026-09-07）
- `.nmod` v1.9：per-function sourceFile（跨文件断点寻址）+ B.1 导入合并补拷 locals（兼修既有 GC 根集洞）；D5 语法修复（SnFunction 产生式锚定 Type——原 @2 NodeFlags 位置 TU 恒 null）
- VM：进程内 `IDebugHooks`（语句/throw 检查点，D6 勘误后三 raise 位点统一 FireOnThrow）+ `IVmDebugView` 只读冻结视图（前端无关，DAP/nide 可复用）；指令打印抽取共享 `Disassembler`（ndisasm golden 逐字节对拍）；D7 行标记去重（LocalDeclStmt 双锚点）
- ndb：断点（file:LINE / LINE / funcName，同行多锚点全设）、步进 s/n/f、bt、frame、info locals（隐藏名过滤）、p、l（SourceCache 三级解析）、x（pc 标记）、catch on|off；初停 gdb start 语义；EOF=q
- 836 e2e（含 9 个 dbg_*）

### 数组类型属性化（数组重设计 B）✅（2026-09-12）
- 属性化：数组值 array-ness 由表达式形状事后推断改为 resolve 期绑定 stamp（`SnExpression::IsArrayValued`，五个表达式形状绑定尾写入）；8 个门位 + `.length` 接收者（resolver/codegen 两侧对称）改查属性；形状推断谓词家族（IsArrayValuedExpr/IsArrayTypedBase）删除——调用点归零
- 三处直修（同根：`EvalDataType()` 对数组字段返回元素类型）：字段 kind 改喂声明类型表达式（`int[]` 存 RTK_Array——writeStruct 静默腐蚀/类字段 toString 报错/GC 欠追踪一家）；MarkPhase 字段显式路由 + 撤运行时 kind 兜底 + 防御性 RTK_Array 元素追踪臂；writeStruct 数组字段死抛错臂激活（具名拒绝）
- 边缘语义具名化：jagged `T[][]` 六声明位点 resolve 期拒绝、foreach 非容器源拒绝（string 源含）、`.length` 任意数组值接收者可解析（`li.get(0).length`/`lib.mk(3).length`）
- `.nmod` v1.10 语义地板（字段 kind 语义变更烤在格式里，旧模块须重编译）；852 e2e / ctest 27+41

### 数组类型一等令牌（0.7.3 表示层根治）✅（2026-09-25）
- 数组类型成为一等驻留令牌（`SnArrayTypeToken` + TU 级驻留表，指针同一即类型同一）：声明侧与值侧共用同一令牌，`IsArrayValued` 由令牌派生；侧信道（`m_bArrayValued`/`GenericArrayFlags` 家族）整族退役，0.7.2 位点级守卫族整删，数组转换裁决统一收敛 GetCastInfo
- 语义收口：协变别名（`Base[] ba = da`）与 `enum[]`↔`int[]` 互通编译期拒；string 强制转换全位置统一（string 形参 / `io.print` / 元素槽）；string 下标编译期拒；foreach 数组值源翻正（源单次求值）；数值二元 / 关系比较 / 条件位 / null 重载平手 / 容器方法值实参全部具名拒绝
- `.nmod` v1.12 布局地板：递归类型描述符（真形参 / 返回 / 字段类型，嵌套数组与 List/Dict，深度帽 8）——跨模块 stub 真签名重建，跨模块调用点类型检查与同模块一致；973 e2e / ctest 46

### 统一库机制（0.8.0 阶段 1–4c）✅（2026-09-29）
- 标准库与第三方库并为一套机制：`stdlib/*.n` 是手写的权威声明，`nlang_<ns>.dll` 经宿主 ABI 提供 native 实现；单段 `import` 命中的库源被**完整内联**为库 TU，限定调用编译为 `OP_CallFunc`，运行时按被调函数自身性质分派（字节码体 / `isNative` → DLL）
- 搜索路径五层统一（`-I`、`.nproj` `<ImportPaths>`、项目目录、`NLANG_PATH`、系统缺省），编译期发现 `.n` 与运行期加载 DLL 共用同一有序列表；nide 叠加工具选项 + 项目属性两级配置
- 库的类型面可用（4b-2）：类型位置写 `ns.Type`，解析器在被内联的库单元内查找 class/struct/enum/interface 并绑定，继承、虚分派、enum 方法与 interface 上行转换同项目类型等价；未 import 的命名空间直接诊断
- 内建标准库退役：硬编码签名表、math/io/fs 三套 intrinsic 家族及其 id 块（70-94、110-114、120-127）全部删除，`OP_CallIntrinsic` 只保留接收者分派的内建方法（string 12 个、流与容器协议）；ctest `no_builtin_stdlib` 守护不留回归路径

### 基本类型完备化（0.7.5）✅（2026-09-30）
- 12 标量基元注册表（byte..ulong 整型家族 + float/double + bool + char，RTK 10..19，单一 PrimitiveTypes 源）；字面量按值域分层、常量适配、混合算术最小容纳提升；有损隐式转换警告 + `ncc --no-warn`
- 严格 bool：比较与谓词产 bool，五处条件位置与 `&&`/`||`/`!` 操作数仅收 bool（`equals` 保持 int 协议例外）
- double 精度默认：无后缀小数与指数字面量 = double；math 浮点族全 double（floor/ceil/round→long）；数值指令改为带 kind 立即数的通用族（函数指针表分派，专用数值 opcode 整族退役）
- char 与 string 桥接：`s[i]` 字节访问（废止 0.7.3 编译期拒）、码点迭代、charAt/charCount/toChar、`\uXXXX` 转义；码点层补齐 Unicode 支持
- 流 64 位原语（writeLong/readLong/writeDouble/readDouble）+ 全基元打印（io.print 扩面 + 单一 OP_Prim_to_str，double 最短往返渲染）
- nide 两级编译器选项（Tools→Options 全局 + 工程属性覆写，首项警告抑制）；ndb 局部变量类型感知渲染 + ndisasm 标量 kind 短名
- `.nmod` v1.13 语义地板（标量 kind 扩表）；1054 e2e（manifest 1034 + 20 示例）/ ctest 50

### 交互输入补全 ✅（0.8.0，master 线）
- `io.write`/`io.eprint` 补齐输出族；宿主 API 新增 `IHostIo::ReadInputLine` 读行原语
- ndb `stdin` 命令向被调试进程注入标准输入；nide 运行输出页内嵌输入行，交互程序无需离开 IDE

### 逐单元产物与加载期链接（0.8.0 阶段 5–6）✅（2026-10-03）
- 产物翻转：单文件编译产出 `.ncu` 单元映像、项目编译打包 `.npkg` 程序包（每源单元一成员＋入口记录）；产物不内联库代码，`.nmod` 退役
- 执行链统一走 nloader→nlink：nloader 沿搜索路径发现并装载引用闭包（版本与成员校验和校验，缺包诊断一次报清）；nlink 纯内存合并（按限定名去重、导入占位槽解析、全操作数重映射）成唯一运行期模块——nvm、`ncc run`、ndb 全部经此链（nide 调用这些工具随之继承）
- 编译器内合并产码链（跨单元导入合并与字节码重映射）整链删除，加载期链接是唯一路径
- `.ncu` v2.0 布局（导入槽＋逐单元模块路径）；合并线抬到 v2.1（1.13/1.14 语义变更并入 2.x，旧 1.x 与合并前 2.0 映像一律拒绝）
- native 动态库按顶层包段命名 `nlang_<package>.dll`；标准库以 `stdlib.npkg` 随工具链分发
- 手册双树围绕逐单元产物模型重写（命令行工具、模块与导入、调试章）

---

## 进行中

- **0.12.x 系列：内建声明真源化收尾＋用户定义泛型**（用户指示 2026-10-28）——数组/字符串声明面进预置包（承接 0.11.x 内建声明真源化两阶段；按class重实现的设想已于 2026-11-01 否决，见远期特性数组实现路线结论），用户定义泛型`class Foo<T>`完整落地（含误拼诊断文案改写：现"not a built-in generic type"误标声明模板臂，v0.12.1 探针已钉该文案）；系列首版 v0.12.0，v0.12.1 闭括号配对修复落地（嵌套泛型`Box<Box<int>>`解锁，探针已翻转）；v0.12.2 数组/字符串声明面统一（预置面类String/Array声明方法面、resolver按声明校验、类型位拒绝门、签名表退役改绑定表取id）；v0.12.3 并发类拆分（thread.n/concurrent.n两包分工，语言核心与基础容器恒可见的§12.4一句话规则）；v0.12.4 const聚合（`const class`/`const struct`类型级不可变＋NCU v2.12标记）与Exception不可变＋cause链＋失败报告沿链输出；v0.12.5 预置包拆分（语言核心进`lang.n`、基础容器进`containers.n`，builtins单体退役，`import builtins;`成为未知包，项目同名源文件即该包副本）；v0.12.6静态成员与静态局部（class/interface静态字段＋函数内静态局部＋`static const`编译期折叠/懒惰初始化、初始化异常可捕获重试、循环import编译期禁止；docs/dev/static_members_design.md）；v0.12.7数组/张量统一（单一类型`Array<T>`＋方括号静态秩标注与边界秩检查、O(1)视图/slice/广播、数组进Object、序列化数组内容＋调试器视图显示、fs二进制读取对，MNIST MLP验收0.897；docs/dev/array_tensor_design.md）；v0.12.8网络库（net包：TCP监听器/流、UDP、ICMP ping、DNS解析作业，ABI v11 IO停泊与工作池回调，docs/dev/net_library_design.md）；v0.12.9合并发布（0.12.8网络库并入主线、0.12.7打标后修复批收编、两轮对抗评审修复批）
- **0.13.0：值相等批＋手册已知限制清偿**（用户裁定2026-11-07：并入v0.13.2第①期深值比较＋泛型struct实参解除，其余期次留v0.13.2）——struct `==`/`!=`按字段值比较（`OP_Eq_struct`/`OP_Ne_struct`，NCU v2.14，比较门拒绝排序/异类型/null）；容器元素与Dict键struct按深值、func按句柄内容三元组匹配（`StructValuesEqual`/`FuncRecordValuesEqual`）；用户模板接受struct类型实参（边界深拷，体内T→T搬运别名为已文档化角落）；func类型作Dict键的门删除；const前缀顶层函数两拼写统一具名拒绝（TopItem文法诊断臂＋`CheckFunctionModifierContract`，冲突台账29+3→30+5）；手册语言页删6条陈旧条目（三条跨模块已由绿测试证伪＋List<struct>/List<func>恒等＋const函数）、VM页删2改1（IntList死引用改`int[]`原生存储）、比较/容器/序列化/字节码页同步值语义；CHANGELOG迁移注记（Dict按值键去重、struct==翻转、dedup写法可用）。
- **0.13.1：运行时泛型类型参数信息**（用户指示2026-11-01，原排0.12.8，网络库占用后于2026-11-07改排）——接通实例化描述符双载体（对象头部＋调用点隐藏实参，宿主ABI v11→v12）＋`typeName(类型表达式)`语言表达式（具体类型编译期折叠为字面量、类型参数`T`在共享方法体内读运行时描述符返回规范拼写string，用户裁定）＋转型按描述符递归比较区分实例化＋catch泛型类型编译期拒绝＋`.ncu`模板表（无源码预编译包铸新实例化）；静态字段补偿模式随本版写入语言规格与用户手册（用户指示2026-11-01）。目标、缺口取证、非目标与验收见docs/dev/generic_runtime_typeinfo_design.md。设计此功能时，应该全面考虑基本的运行时类型信息和辅助设施的支持。
- **0.13.1a：泛型完善** 静态引用T全面化+游离函数泛型支持，实现帧隐藏描述符槽与调用点隐藏实参。
- **v0.13.2：值类型引用（&）**（用户指示 2026-11-07）——参考概要设计经用户/编译器专家/系统架构师三视角审核（docs/dev/value_type_reference_design.md）：值类型（标量/enum/struct/string）保持值语义默认、以`&`显式引入引用（局部/形参/返回/foreach四位置；class等身份引用类型上`&`为等价记号）；统一表示（基准记录句柄＋cell偏移）＋记录稳定化（赋值/写入原位更新，堆分配收敛到新位置建立场景）；out参数退役、`&`形参全面取代；用户泛型struct实参解除；容器元素匹配改字段深值比较（配合项，销账已知限制第26/4项）。四期实施：①深值比较＋泛型解除→②记录稳定化→③&表示＋容器getter→④out退役，每期独立停靠，现行值语义测试全数不翻为护栏；并发原位拷贝按单原子存储操作契约核验。
- **v0.13.3：Lambda函数/闭包**
- **v0.13.4：字符串处理增强/URL库**（已完成，2026-11-08发布）——String三方法（`indexOf(t,from)`/`toInt(radix)`/`equalsIgnoreCase`，VM内建intrinsic）、`io` Base64四编解码、`system`包（`wallMs`/`monoMs`双时钟＋`DateTime` UTC日历类型，新native模块`nlang_system`）、`url`包（RFC 3986 `Url` parse/resolve/toString/queryParams/encodeQuery＋percent/form组件编解码）；null退出string值域＋`string.EMPTY`常量（用户裁定；已知限制：跨native边界null句柄抛异常）；`io.nowMs`迁`system.monoMs`；docs/dev/http_library_design.md
- **v0.13.5：HTTP库**（已完成，2026-11-09）——纯NLang的`http`包（消息模型`HttpRequest`/`HttpResponse`/`HttpHeaders`＋加固`HttpParser`、`HttpServer`每连接一虚拟线程＋400/408/413/417/431/500/503/505加固应答＋`Expect`/100-continue、`HttpClient`按origin连接池＋重定向＋连接/读取/整体三档独立超时＋默认头、requests式自由函数`get`/`post`/`put`/`del`/`request`、`HttpException`携响应`status`）；流式正文（`HttpBodyReader`的`read`/`readAll`/`bodyText`/`close`/`drain`、有界`Channel<byte[]>`＝背压、写失败→关通道→`ChannelClosedException`＝取消）；`gzip`包（整块缓冲`compress`/`decompress`，新native模块`nlang_gzip`内置zlib）；示例`http_hello.n`/`http_conc_bench.n`/`http_sse.n`＋并发验收套件（百次压测）；手册新增第23/24章、格式章重编号为25/26/27；docs/dev/http_library_design.md
- **v0.13.6：net/http库性能优化** 1）修复HTTP库实施期间发现的语言面缺口。2）性能门槛（用户裁定2026-11-10：与Node.js处于同一数量级，即同机同参数吞吐比值不超过10倍，单连接与高并发两行同时判定，未达门槛不得收版）本版收口时达标：主表单连接1814 RPS、高并发5907 RPS，与同窗Node.js的吞吐比值0.17与0.35，两行均在门槛内；高并发行的表观下降经三body档同探针复测（git `ee84a593`）判为整机窗口漂移而非代码回归，1 KiB跨版本落差属基线文档已钉死的「量子上限退场后正文成本显形」形态。判定口径见docs/dev/net_http_perf_design.md§一与§七，五方对拍（nginx、node、Go、Rust主流生态）数据与可重跑设施在docs/dev/http_perf_baseline.md与tools/http_perf/。3）`http.n`每响应的两处已定位成本已清偿：响应头与正文合并成一次发送（nginx与hyper都对小正文合并），`Date`头按秒缓存、不再每次重格式化（原实现含两次数组分配与13次串接）；配对A/B复测收益记在docs/dev/http_perf_baseline.md§5末与§6。4）`TcpStream.readLine`的缓冲归属已裁定并落地（D5）：缓冲归`TcpStream`自有的单个8192字节惰性接收窗口，`read`/`readLine`/`readExactly`先消费窗口、行长超过窗口的行跨补窗装配；`http.n`行读取器自带的`FILL_WINDOW`改为通过新增公开面`peekFilled(minBytes = 1, timeoutMs = 0)`与`consume(count)`读取和消费。逐字节解码串接已在修复批消除（同机同一次锁内对拍，64 KiB单行459 ms→243 ms），逐字节一次系统调用的形态随D5接收窗口消除（同输入按8 KiB块读的85 ms档达成）。5）`net.ping`的`timeoutMs = 0`（不设截止时刻）按D6处理：无截止的ping不再排进优先级1的原生工作池，不再推迟优先级0的主机名解析；完全走兴趣表停泊的根治形态随D8递延（Windows的`IcmpSendEcho2`句柄不是可轮询描述符，等平台侧等待形态定案），当前契约已在§21.4写明。6）net包的POSIX实现此前从未编译过（本机无Linux CI），本版已清偿编译期缺口——`net_icmp.cpp`include了只在Windows可用的`net_platform_internal.h`来读线程局部错误文本（改走跨平台访问器`NetTakeLastErrorOverride()`：Windows交出并清除该文本，POSIX无覆盖恒返空串）、`net_platform_posix.cpp`落后于`net_platform.h`的`NetResolve`/`NetIcmpEcho`参数与缺失的`NetResolveErrorText`定义；源码规模守卫同时暴露`net_platform_posix.cpp`超500行，按Windows的布局把ICMP条目拆成独立的`net_platform_icmp_posix.cpp`，两个POSIX翻译单元共用的套接字助手移入新的`net_platform_posix_internal.h`——并新增`tools/posix_net_build_check.sh`（Linux或WSL下编译该模块的Linux源文件清单并核对符号，判决行`GATE posix_net_build: PASS`）；这只证明编译与符号面，运行时仍未验；net族e2e的Linux实跑与该检查接入门禁，按用户裁定（2026-11-10）挪到nlang整体支持Linux时一并做，本版不做（docs/dev/net_http_perf_design.md裁定D8）。7）超过`maxConnections`的连接拒绝（`http.n`的`reject503`）按D7裁定：不再在accept线程上同步写完503再有序关闭，交给短工线程处理，连接风暴期间一批拒绝不再推迟其后的accept；在途拒绝已达64时超量连接直接关闭而无应答，客户端读到空响应，两种形态都写进手册`maxConnections`说明并有e2e覆盖后一种。
- **v0.13.7：合并和正则表达式库**  1）合并master和dev分支修改；2）实现正则表达式库。
- **v0.13.8：跨模块功能完善** 1）支持跨模块同参数数重载，同名同参数数但参数类型不同的两个重载函数。3）解决枚举不能跨模块导入的已知限制。4）解决导入struct的实例字段跨模块访问的已知限制。5）跨模块游离函数和类的可见性完善，支持private/protected/public访问控制：类缺省public；函数缺省protected（同一package内可见）。
- **v0.13.9：WEB框架**——基于`http`包的web框架（路由/中间件/session/静态资源＋SSE helper，本项目只给HTTP分帧能力不解释SSE语义）；随本版一并清偿HTTP库收尾两项：服务器优雅关闭（连接登记表＋排空/强关，含`HttpServer.close()`后重新`start()`/`serve()`不复位关闭标志的复位项）、net`TcpStream.read`区分超时与对端EOF

## 远期特性

- lambda 表达式 + 闭包捕获；enum 方法引用；跨模块函数引用；多播委托；Func 协变/逆变
- 底层实现的内存管理全面 RAII 化：智能指针替换文法/构建器中的裸 `new` 与自定义 `UniquePtr` 容器（EnNew/EnDelete 宏已于 2026-08-23 移除）
- 基于LLVM的本地字节码生成编译器
- 包管理器：模块依赖管理（阶段 11 延后项）
- 进程内链接 nlang_vm 的 C/C++ 宿主 API：把编译执行能力以库形式嵌入应用程序（当前集成形态为子进程驱动）
- 调试信息完善：编译期记录每个函数的源文件名（编译产物内字段，或随包产出编译附加信息文件），让回溯与断点显示真实来源——当前回溯帧以入口单元文件名搭配各函数行号，标准库帧会错位
- 数组实现路线结论（2026-11-01设计裁定，0.12.7已落地单一类型`Array<T>`）——否决「按class重实现」：代价为元素宽度规则冲突（字段统一8字节步长vs数组按种类4/8）与记录消费者横切特例；声明面与虚派发的收益已由预置面类＋`Array<T>`真类型拿到，如要重开须单独立项
- 数组特性延后项（0.12.7裁定登记，见docs/dev/array_tensor_design.md D14）：负标量下标（`a[-1]`＝len+neg换算，已裁定支持，牵连List/string下标一致性故实现靠后）；max/min沿轴归约（`ReduceAxisKernel`模板已随sum(axis)落地，有消费者时按需开启）；批量matmul（前导维广播语义）；slice表达式作赋值目标（视图左值机制）
- HTTP库实施期间的语言面缺口（2026-11-09实测登记）：文件级变量不能与`main()`并存（现报"top-level statements cannot be combined with an explicit main() function"，共享计数器只能塞进只装静态字段的持有类）；类类型形参的实参不能写`f(null)`（须先声明一个值为null的局部变量再传）；同类`static const`不能用于实例字段初始化器。三者都是普通程序员会期待的行为，按合理性优先排期，不记入手册「已知限制」
- 字符串维持特化存储＋装箱进`Object`（2026-11-01设计结论，取代原「基于class的字符串实现」）：字符串存放在独立存储中，句柄与堆索引共用一个空间因而冲突，装箱是唯一不动物理布局的一致性方案（Go的`any`对字符串同样是装箱）；免装箱的替代路线（并入对象堆、带标签引用）成本远超收益，如要评估须单独立项
- LSP 支持：VS Code / JetBrains 协议（用户指示 2026-08-21：最后实施）
- Linux/macOS 打包发布：Windows zip + NSIS（CPack）已落地（2026-08-26）；Linux 暂缓，待源码跨平台移植修复后以 CI 构建 TGZ/DEB
- 安装包组件化（tools-only / IDE-only）与捆绑 VC++ 运行库（/MT 或 vc_redist）
- 文档子系统延后项：①片段审计扩展到 specification/vm-architecture 的**运行**审计（约 68 个 bare 围栏块的**标签分类**已由 2026-08-30 导航细化+语法高亮轮吸收；运行审计仍延后）；②站点语言政策决策（含用户 2026-08-30 指示的中英双站方向——机制选型 mkdocs-static-i18n vs 每 locale 独立 build，后者对 file:// 离线更稳）

---

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P4 | LSP 支持 | 最后实施（用户指示 2026-08-21） |

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/user_manual/{zh,en}/specification/ | NLang 语言规范（语言、标准库、二进制格式、参考页），带 § 条款编号，中英双树各 70 页 |
| docs/user_manual/{zh,en}/vm-architecture/ | VM 架构设计（编译管线、堆布局、GC 算法、指令集、库机制），一章一页 16 篇，中英双树 |
