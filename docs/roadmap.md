# NLang Roadmap

## 项目背景

NLang 是一门独立的静态类型脚本语言，配有字节码编译器和虚拟机。语法和早期实现受 EN 引擎中 `compiler_bak/`（编译器）和 `lang_bak/`（VM）启发，但作为独立语言演进，不再以 EN 集成为目标。

历史参考：
- EN 引擎位置：`E:/cases/en/src/common/compiler_bak/`、`E:/cases/en/src/common/lang_bak/`
- EN IDE 位置：`E:/cases/en/src/tools/nide/`（基于 Qt 的 IDE）

> 各阶段的实施细节（commit 号、opcode 清单、陷阱记录）归档于 git 历史、各阶段计划文件（`~/.claude/plans/`）与项目 memory；本文件只保留阶段成果摘要。

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
- 计划：`~/.claude/plans/partitioned-roaming-garden.md`

### 调试器 ndb ✅（2026-09-07）
- `.nmod` v1.9：per-function sourceFile（跨文件断点寻址）+ B.1 导入合并补拷 locals（兼修既有 GC 根集洞）；D5 语法修复（SnFunction 产生式锚定 Type——原 @2 NodeFlags 位置 TU 恒 null）
- VM：进程内 `IDebugHooks`（语句/throw 检查点，D6 勘误后三 raise 位点统一 FireOnThrow）+ `IVmDebugView` 只读冻结视图（前端无关，DAP/nide 可复用）；指令打印抽取共享 `Disassembler`（ndisasm golden 逐字节对拍）；D7 行标记去重（LocalDeclStmt 双锚点）
- ndb：断点（file:LINE / LINE / funcName，同行多锚点全设）、步进 s/n/f、bt、frame、info locals（隐藏名过滤）、p、l（SourceCache 三级解析）、x（pc 标记）、catch on|off；初停 gdb start 语义；EOF=q
- 836 e2e（含 9 个 dbg_*）；设计/计划：docs/superpowers/{specs,plans}/2026-09-06-nlang-debugger*

### 数组类型属性化（数组重设计 B）✅（2026-09-12）
- 属性化：数组值 array-ness 由表达式形状事后推断改为 resolve 期绑定 stamp（`SnExpression::IsArrayValued`，五个表达式形状绑定尾写入）；8 个门位 + `.length` 接收者（resolver/codegen 两侧对称）改查属性；形状推断谓词家族（IsArrayValuedExpr/IsArrayTypedBase）删除——调用点归零
- 三处直修（同根：`EvalDataType()` 对数组字段返回元素类型）：字段 kind 改喂声明类型表达式（`int[]` 存 RTK_Array——writeStruct 静默腐蚀/类字段 toString 报错/GC 欠追踪一家）；MarkPhase 字段显式路由 + 撤运行时 kind 兜底 + 防御性 RTK_Array 元素追踪臂；writeStruct 数组字段死抛错臂激活（具名拒绝）
- 边缘语义具名化：jagged `T[][]` 六声明位点 resolve 期拒绝、foreach 非容器源拒绝（string 源含）、`.length` 任意数组值接收者可解析（`li.get(0).length`/`lib.mk(3).length`）
- `.nmod` v1.10 语义地板（字段 kind 语义变更烤在格式里，旧模块须重编译）；852 e2e / ctest 27+41；设计/计划：docs/superpowers/{specs,plans}/2026-09-10-array-*


---

## 进行中

（无）

## 远期特性

- lambda 表达式 + 闭包捕获；enum 方法引用；跨模块函数引用；多播委托；Func 协变/逆变
- 用户定义泛型：`class Foo<T>`
- 底层实现的内存管理全面 RAII 化：智能指针替换文法/构建器中的裸 `new` 与自定义 `UniquePtr` 容器（EnNew/EnDelete 宏已于 2026-08-23 移除）
- 基于LLVM的本地字节码生成编译器
- 包管理器：模块依赖管理（阶段 11 延后项）
- 基于class的数组实现（非动态容量，但是属性和方法重用class的机制）
- 基于class的字符串实现（非动态容量，但是属性和方法重用class的机制）
- foreach 支持string遍历（阶段 8e 延后项）
- Unicode字符串支持（阶段 8e 延后项）
- LSP 支持：VS Code / JetBrains 协议（用户指示 2026-08-21：最后实施）
- Linux/macOS 打包发布：Windows zip + NSIS（CPack）已落地（2026-08-26，设计见 docs/superpowers/specs/2026-08-26-release-packaging-design.md）；Linux 暂缓，待源码跨平台移植修复后以 CI 构建 TGZ/DEB
- 安装包组件化（tools-only / IDE-only）与捆绑 VC++ 运行库（/MT 或 vc_redist）
- 文档子系统延后项：①片段审计扩展到 language-spec/vm-architecture 的**运行**审计（约 68 个 bare 围栏块的**标签分类**已由 2026-08-30 导航细化+语法高亮规格吸收，见 docs/superpowers/specs/2026-08-30-docs-nav-and-highlight-design.md §5；运行审计仍延后）；②站点语言政策决策（含用户 2026-08-30 指示的中英双站方向：exit-code-convention.md 为中文而 13 篇同级章节页为英文——机制选型 mkdocs-static-i18n vs 每 locale 独立 build，后者对 file:// 离线更稳，见同规格 §7）

---

## 当前状态

- **852 个 e2e 测试全绿**（`tests/e2e/run_e2e_tests.py`，含 12 个 dbg_*/dbgm_* 调试器 e2e）；ctest 27 项（build 树）/ 41 项（build-ide 树）
- 工具链：ncc / nvm / ndisasm / ndb（调试器）/ nide（Qt5）全部可用；C++17 + CMake 3.16+，支持离线构建部署
- 模块格式 v1.10（v1.8 Func 句柄 + per-function sourceFile 调试器寻址；v1.10 语义地板：数组 struct/class 字段 kind 存 RTK_Array——旧模块须重编译）
- 语言面：完整过程式 + OOP（继承/虚方法/接口）+ 泛型容器 + 异常 + 原生绑定 + 标准库 + 一等函数值（Func/委托）+ 类型别名
- 已知遗留：bare `[]` 空 init、bare init list 作函数实参、native 参数列集与签名校验（9f-2）、`List < 3` shadow 比较、继承 ctor 在 `new` 调用点不支持；数组值检测残余（属性化后仍开的洞）——泛型实例化键擦除（List<T[]> 与 List<T> 共享实例化键，键只记首个数组实参，Dict 数组键遮蔽数组值）：List<T[]> 元素经 BoxingTagFor 装进 RTK_Int32 标签的箱子（元素字段 masquerade 为 NK_Int32）→ 载荷结构性不可追踪、被 GC 清扫，读残留位 Release 巧合幸存（Sweep clear() 不清位 + freeList LIFO 复用不触底 + 元素 opcode 不查槽 kind——幽灵读），ASan 下为 container-overflow（2026-09-12 实测）；invoke 形基座 `mk().get(0)` 同根因（ContainerElemIsArray 不识别 invoke 形 Outer），今日实测可用且有 e2e `call_result_get_positive` 行为钉——真正的洞是 `List<int[]>` invoke 形基座，并入本条；均归 C 期修复、foreach 数组型循环变量 over List<T[]> 不解析——根因主为 resolve 路径缺口：数组类型循环变量的类型表达式经 StatementResolver visitor 解析，SnArrayTypeExpr 命中空 Access() 永不 resolve（任意深度数组类型循环变量声明「半坏」：体/条件引用之 → "Cannot resolve the field"；不引用 → 静默编译通过），泛型擦除为次因，归 C 期或独立线、数组元素 opcode（OP_LoadElement/StoreElement/ArrayLength）不校验槽 kind——kind-blind 读使 GC 追踪回归在 Release 下幽灵绿（ASan 可显形），kind 校验加固为独立候选、B.1 导入合并不拷贝 defaultValues（executor 零消费——纯编译期数据，调用点内联；但消费方再导出 .nmod 的链路默认参数会丢）

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P4 | LSP 支持 | 最后实施（用户指示 2026-08-21） |

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/language-spec/ | NLang 语言规范（类型语义、语法、GC 行为），一章一页 14 篇 |
| docs/vm-architecture/ | VM 架构设计（编译管线、堆布局、GC 算法、指令集），一章一页 13 篇 |
