# NLang Roadmap

## 项目背景

NLang 是一门面向嵌入与自动化场景的静态类型脚本语言，配有字节码编译器和虚拟机，并作为探索 AI 友好语言特性的试验台演进。

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

### 统一库机制（0.7.9 阶段 1–4c）✅（2026-09-29）
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

### 交互输入补全 ✅（0.7.9，master 线）
- `io.write`/`io.eprint` 补齐输出族；宿主 API 新增 `IHostIo::ReadInputLine` 读行原语
- ndb `stdin` 命令向被调试进程注入标准输入；nide 运行输出页内嵌输入行，交互程序无需离开 IDE

### 逐单元产物与加载期链接（0.7.9 阶段 5–6）✅（2026-10-03）
- 产物翻转：单文件编译产出 `.ncu` 单元映像、项目编译打包 `.npkg` 程序包（每源单元一成员＋入口记录）；产物不内联库代码，`.nmod` 退役
- 执行链统一走 nloader→nlink：nloader 沿搜索路径发现并装载引用闭包（版本与成员校验和校验，缺包诊断一次报清）；nlink 纯内存合并（按限定名去重、导入占位槽解析、全操作数重映射）成唯一运行期模块——nvm、`ncc run`、ndb 全部经此链（nide 调用这些工具随之继承）
- 编译器内合并产码链（跨单元导入合并与字节码重映射）整链删除，加载期链接是唯一路径
- `.ncu` v2.0 布局（导入槽＋逐单元模块路径）；合并线抬到 v2.1（1.13/1.14 语义变更并入 2.x，旧 1.x 与合并前 2.0 映像一律拒绝）
- native 动态库按顶层包段命名 `nlang_<package>.dll`；标准库以 `stdlib.npkg` 随工具链分发
- 手册双树围绕逐单元产物模型重写（命令行工具、模块与导入、调试章）

---

## 进行中

（无）

## 远期特性

- lambda 表达式 + 闭包捕获；enum 方法引用；跨模块函数引用；多播委托；Func 协变/逆变
- 用户定义泛型：`class Foo<T>`
- 底层实现的内存管理全面 RAII 化：智能指针替换文法/构建器中的裸 `new` 与自定义 `UniquePtr` 容器（EnNew/EnDelete 宏已于 2026-08-23 移除）
- 基于LLVM的本地字节码生成编译器
- 包管理器：模块依赖管理（阶段 11 延后项）
- 进程内链接 nlang_vm 的 C/C++ 宿主 API：把编译执行能力以库形式嵌入应用程序（当前集成形态为子进程驱动）
- 调试信息完善：编译期记录每个函数的源文件名（编译产物内字段，或随包产出编译附加信息文件），让回溯与断点显示真实来源——当前回溯帧以入口单元文件名搭配各函数行号，标准库帧会错位
- 基于class的数组实现（非动态容量，但是属性和方法重用class的机制）
- 基于class的字符串实现（非动态容量，但是属性和方法重用class的机制）
- LSP 支持：VS Code / JetBrains 协议（用户指示 2026-08-21：最后实施）
- Linux/macOS 打包发布：Windows zip + NSIS（CPack）已落地（2026-08-26）；Linux 暂缓，待源码跨平台移植修复后以 CI 构建 TGZ/DEB
- 安装包组件化（tools-only / IDE-only）与捆绑 VC++ 运行库（/MT 或 vc_redist）
- 文档子系统延后项：①片段审计扩展到 language-spec/vm-architecture 的**运行**审计（约 68 个 bare 围栏块的**标签分类**已由 2026-08-30 导航细化+语法高亮轮吸收；运行审计仍延后）；②站点语言政策决策（含用户 2026-08-30 指示的中英双站方向——机制选型 mkdocs-static-i18n vs 每 locale 独立 build，后者对 file:// 离线更稳）

---

## 当前状态

- **1066 个 e2e 测试全绿**（`tests/e2e/run_e2e_tests.py`，manifest 1046 项 + 20 个示例）；ctest 74 项（build-dev 树）
- 工具链：ncc / nvm / ndisasm / ndb（调试器）/ nide（Qt5）全部可用；C++17 + CMake 3.16+，支持离线构建部署
- 模块格式 `.ncu` v2.1（加载期链接布局：导入槽＋逐单元模块路径；1.13/1.14 语义变更——标量 kind 扩表与局部声明作用域——并入 2.x 线，旧 1.x 与合并前 2.0 映像一律拒绝）＋ `.npkg` v1.0 程序包（成员校验和）；`.nmod` 已退役
- 语言面：完整过程式 + OOP（继承/虚方法/接口）+ 泛型容器（类型实参允许 `T[]`）+ 异常 + 原生绑定 + 标准库与第三方库统一包机制（显式 import、限定路径调用）+ 一等函数值（Func/委托）+ 类型别名 + 一等数组类型令牌 + 12 标量基元家族（严格 bool、double 默认、char 码点桥）
- 已知遗留：bare `[]` 空 init、bare init list 作函数实参、native 参数列集与签名校验（9f-2）、`List < 3` shadow 比较、继承 ctor 在 `new` 调用点不支持、共享命名空间跨目录三形式不可达（v1 例外）

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P4 | LSP 支持 | 最后实施（用户指示 2026-08-21） |

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/user_manual/{zh,en}/language-spec/ | NLang 语言规范（类型语义、语法、GC 行为），一章一页 16 篇，中英双树 |
| docs/user_manual/{zh,en}/vm-architecture/ | VM 架构设计（编译管线、堆布局、GC 算法、指令集、库机制），一章一页 15 篇，中英双树 |
