# NLang Roadmap

## 项目背景

NLang 是一门独立的静态类型脚本语言，配有字节码编译器和虚拟机。语法和早期实现受 EN 引擎中 `compiler_bak/`（编译器）和 `lang_bak/`（VM）启发，但作为独立语言演进，不再以 EN 集成为目标。

历史参考：
- EN 引擎位置：`E:/cases/en/src/common/compiler_bak/`、`E:/cases/en/src/common/lang_bak/`
- EN IDE 位置：`E:/cases/en/src/tools/nide/`（基于 Qt 的 IDE）

> 各阶段的实施细节（commit 号、opcode 清单、陷阱记录）归档于 git 历史、各阶段计划文件（`C:\Users\dliting\.claude\plans\`）与项目 memory；本文件只保留阶段成果摘要。

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

---

## 进行中

### 阶段 13：函数/方法代理 + 类型别名（2026-08-23 启动）
- `Func<返回, 参数...>` 内建泛型函数类型（返回在前、参数在后，参数可带 out；精确签名匹配）
- 函数引用与绑定方法引用作为一等值：局部/参数/字段/容器存储，`f(x)` 调用；虚/接口方法运行时派发
- `using Name = Type;` 类型别名
- 计划：`C:\Users\dliting\.claude\plans\partitioned-roaming-garden.md`

## 远期特性

- lambda 表达式 + 闭包捕获；enum 方法引用；跨模块函数引用；多播委托；Func 协变/逆变
- 用户定义泛型：`class Foo<T>`
- 包管理器：模块依赖管理（阶段 11 延后项）
- LSP 支持：VS Code / JetBrains 协议（用户指示 2026-08-21：最后实施）
- 数组动态增长重设计（Python list 风格，基于 class）

---

## 当前状态

- **687 个 e2e 测试全绿**（`tests/e2e/run_e2e_tests.py`）；ctest 9 项（编译器/VM）+ IDE 17 项
- 工具链：ncc / nvm / ndisasm / nide（Qt5）全部可用；C++17 + CMake 3.16+，支持离线构建部署
- 模块格式 v1.7（native 标志 + 标准库返回种类）；Phase 13 将升至 v1.8（Func 句柄）
- 语言面：完整过程式 + OOP（继承/虚方法/接口）+ 泛型容器 + 异常 + 原生绑定 + 标准库
- 已知遗留：bare `[]` 空 init、bare init list 作函数实参、native 参数列集与签名校验（9f-2）、`List<T[]>` 容器内数组绕接收者门、`List < 3` shadow 比较、继承 ctor 在 `new` 调用点不支持、`tests/e2e/run_all.py` 旧 runner 待清理

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P1 | 13. 函数/方法代理 + 类型别名 | 进行中（2026-08-23） |
| P2 | 横切收尾 | IsArrayValuedExpr 容器元素臂（关接收者门与 switch 族门的 List<T[]> 残余） |
| P4 | LSP 支持 | 最后实施（用户指示 2026-08-21） |

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/language-spec.md | NLang 语言规范（类型语义、语法、GC 行为） |
| docs/vm-architecture.md | VM 架构设计（编译管线、堆布局、GC 算法、指令集） |
