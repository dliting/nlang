# NLang Roadmap

## 项目背景

NLang 是一门独立的静态类型脚本语言，配有字节码编译器和虚拟机。语法和早期实现受 EN 引擎中 `compiler_bak/`（编译器）和 `lang_bak/`（VM）启发，但作为独立语言演进，不再以与 EN 集成为目标。

历史参考：
- EN 引擎位置：`E:/cases/en/src/common/compiler_bak/`、`E:/cases/en/src/common/lang_bak/`
- EN IDE 位置：`E:/cases/en/src/tools/nide/`（基于 Qt 的 IDE）

---

## 已完成功能

### 阶段 0：基础框架 ✅
- 编译器框架：AST 节点体系、Visitor 模式、X-macro 自动生成
- 语法解析器：flex/bison 生成，基础语法规则
- VM 后端：VmBackend 代码生成、BytecodeEmitter/Reader
- VM 执行器：VmExecutor 指令执行、局部变量帧
- 工具链：ncc（编译器）、nvm（虚拟机）、ndisasm（反汇编器）
- 模块系统：.nmod 文件保存/加载、模块导入

### 阶段 1：基础语言特性 ✅
- 数据类型：int32、float、string
- 变量声明与赋值：`int x = 5;`、`x = 10;`
- 算术运算：+、-、*、/、%、一元负
- 比较运算：==、!=、<、>、<=、>=
- 逻辑运算：&&、||、!
- 类型转换：int↔float 隐式/显式转换
- 控制流：if/else、while、do-while、for、break、continue
- 函数：定义、调用、参数传递、递归、返回值
- 字符串：字面量、拼接、比较、length()、参数传递、返回值

### 阶段 2：复合类型 ✅
- 枚举：enum 声明、成员访问（Color.Red）、switch/case、enum→int 转换
- switch/case 语句：default、break、循环中的 switch
- 结构体：struct 声明、字段访问（pt.x）、字段赋值、函数参数（值传递深拷贝）、返回值、嵌套结构体

### 阶段 3：类和对象 ✅
- 类声明与实例化：`class Foo { }`、`new Foo()`
- this 指针：方法体内通过 this 访问自身成员
- 成员方法：类内定义函数，隐式接收 this 参数
- 引用语义：对象变量存储堆索引，赋值/传参是引用传递
- 构造函数：`Foo() { this.x = 0; }` 可选参数初始化
- 继承：`class Dog : Animal { }` 单继承，子类继承父类字段和方法
- 虚方法：`virtual` 关键字，运行时名称查找分派
- 方法重写：子类重写父类虚方法，动态分派
- 访问控制：public/private/protected
- null 值：对象可为 null，访问 null 引用抛出运行时错误（fail-fast）
- struct 作为 class 字段：class 拥有独立的 struct 副本（值语义深拷贝）

### 阶段 5：struct-class 互嵌 + 垃圾回收 ✅
- struct 包含 class 字段：class 引用浅拷贝（只拷贝堆索引，不拷贝对象）
- class 包含 struct 字段：struct 深拷贝（class 拥有独立副本）— 阶段 3 已实现
- 标记-清除 GC：安全点触发、精确扫描、空闲列表复用
- GC 设计决策（详见 VmExecutor.h 注释和 docs/vm-architecture.md）：
  - 安全点触发（函数入口 + 循环回边），非分配点触发
  - 精确扫描（基于 LocalDescriptor），非保守扫描
  - m_slotStructIdx 并行数组标识 struct 类型

### 阶段 4：数组 ✅
- 固定数组：`int[10] arr;`、`arr[i]`、`arr[i] = val`、`arr.length()`
- 数组作为函数参数（引用传递）、数组作为类字段
- 多维嵌套数组、运行时大小数组
- 元素类型覆盖 int/float/string/class/struct/array/null

### 阶段 6：接口与多态 ✅
- 接口声明：`interface IFoo { int Bar(); }`
- 类实现接口、接口类型变量、接口方法调用（vtable 分派）
- 接口继承、多接口实现、null 接口引用

### 阶段 7：调试支持（backtrace）✅
- 字节码嵌入源码行号、运行时调用栈回溯
- NullPointer / 异常路径自动打印 backtrace

### 阶段 8：序列化与持久化 ✅
- ByteStream / FileStream 内建类
- 8a：流读写 int/float/string
- 8b：struct 序列化（深拷贝，含嵌套 struct/string/float 字段）
- 8c：class 字段序列化（含 null、shared ref、cycle、继承、深链）
- 8d：顶层 class 序列化 + 多态（object graph roundtrip）
- 引用解析：shared ref 复用、cycle 检测、跨流恢复

### 阶段 8e-1：Object 基类 + 哈希/相等协议 + 隐式装箱 ✅
- 隐式 Object 基类：所有不带 `: Parent` 的类自动继承 Object
- 虚方法 `int Equals(Object)` 和 `int GetHashCode()`，默认身份语义（identity）
- 字符串特例：`string.GetHashCode()` 值哈希、`string.Equals(string)` 值相等
- 用户类按名称重写 Equals/GetHashCode（无需 override 关键字，沿用既有名称分派）
- 基本类型隐式装箱：`Object o = 5;`、`Object f = 3.14;`、`Object s = "hi";`（新增 RTK_Boxed=6 槽位类型，GC MarkPhase 显式跳过）
- 解析器承认 `Object` 为内建类型名（与 ByteStream/FileStream 并列）

### 阶段 8e-1.5：显式拆箱 / 类向下转型 ✅
- 显式拆箱：`int x = o as int;`（运行时类型检查，不匹配抛异常）
- 类向下转型：`Point p = obj as Point;`（运行时类型检查）
- **设计决策：** 使用 `as` 关键字而非 `(T)expr` 前缀，避免 LALR(1) 解析器与括号表达式冲突
- 新增 AST 节点 SnAsExpr (NK_AsExpr)，新词法 token KT_As，新文法规则 `Expression KT_As NameExpr`
- TypeCastInfo 扩展 TCK_Unbox 和 TCK_Downcast；新指令 OP_Unbox / OP_CheckCast
- **关键修复：** CastInfo.cpp 中 TCK_Box 检查顺序提前到 null-literal TCK_Auto 规则之前（修复 `Object o = 5` 不触发装箱的潜在 bug）
- **Object 特殊化：** AST 层 SnClassDecl::SuperClass() 链不含隐式 Object 父类（只有 CompiledClass.superClassIdx 含），CastInfo 必须按名称特判 `target=="Object"`（upcast→TCK_Same）和 `source=="Object"`（downcast→TCK_Downcast）
- **null 保留：** OP_Box 对输入值 0（null 字面量）跳过堆分配，直接写回 0，保留 `Object o = null` 的 no-op 语义
- 6 个新 e2e 测试（含 unbox_float_and_string），共 184 个测试全部通过

---


## 后续阶段

### 阶段 4：数组

**EN 参考：** I_Base_ArrayMember（NInstructBase.h:57）、I_Base_VectorMember/VectorLength/VectorInsert/VectorAppend/VectorRemove/VectorFind（NInstructBase.h:58-64）

**核心特性：**
- 固定数组：`int[10] arr;` 或 `int arr[10];`
- 数组访问：`arr[i]`（下标表达式）
- 数组赋值：`arr[i] = 5;`
- 数组长度：`arr.length()` 或 `arr.size`
- 数组作为函数参数：引用传递（传堆索引）
- 动态数组（vector）：`int[] arr; arr.push(1); arr.remove(0);`
- 数组作为类字段：对象中包含数组字段

**设计要点：**
- 数组变量存储堆索引（类似 struct），指向 m_arrayHeap 中的连续数据区
- 数组元素按 VALUE_SIZE 对齐，下标访问通过偏移计算
- 动态数组需要长度字段 + 容量字段 + 数据区

**新增 VM 指令：**
- `OP_AllocArray <uint16 dst> <uint16 elemTypeKind> <uint16 count>` — 分配固定大小数组
- `OP_LoadElement <uint16 dst> <uint16 arr> <uint16 idx>` — arr[idx] 读取
- `OP_StoreElement <uint16 arr> <uint16 idx> <uint16 src>` — arr[idx] = val 写入
- `OP_ArrayLength <uint16 dst> <uint16 arr>` — 获取数组长度

### 阶段 6：接口与多态

**EN 参考：** I_Base_InterfaceMember（NInstructBase.h:50）、I_Base_InterfaceCast（NInstructBase.h:30）、NInterface（lang_bak/intf/NInterface.h）

**核心特性：**
- 接口声明：`interface IMovable { void move(int dx); }`
- 接口实现：`class Player : IMovable { void move(int dx) { ... } }`
- 接口类型变量：`IMovable m = new Player(); m.move(1);`
- 接口方法调用：通过接口 vtable 分派
- 动态类型转换：`obj as Player` 安全转换
- 类型检查：`obj instanceof Player`（可选）

### 阶段 7：调试支持

**EN 参考：** I_Base_DebugInfo（NInstructBase.h:67）

**核心特性：**
- 调试行号信息：编译器在字节码中嵌入源码行号映射
- 断点支持：VM 在指定行号处暂停执行
- 单步执行：逐语句/逐过程/逐出
- 变量查看：暂停时读取局部变量、this 成员、字段值
- 调用栈回溯：函数调用链、每层参数值

### 阶段 8：序列化与持久化

**EN 参考：** Archive（lang_bak/intf/Archive.h）

**核心特性：**
- 对象序列化：将对象图保存到二进制流
- 对象反序列化：从二进制流恢复对象图
- 引用解析：序列化时记录对象引用关系，反序列化时恢复
- 版本兼容：字段增删时的向前/向后兼容

### 阶段 9：高级语言特性

- 增量赋值：`x += 1;`、`x -= 1;`
- 默认参数：`int foo(int x, int y = 0)`
- out 参数：`void foo(int x, out int y)`
- for-each 循环：`for (int x in arr) { ... }`
- 字符串插值：`"Hello ${name}"`
- 异常处理：try/catch/throw
- 常量修饰：`const int X = 5;`
- 原生函数绑定：`native void foo();`
- 断言：`assert(condition);`
- 代理/回调：函数指针、委托类型
- 泛型（远期）

### 阶段 10：IDE 移植（nide）

**EN 参考：** nide（E:/cases/en/src/tools/nide/），基于 Qt 5.15 + Widgets

**核心特性：**
- 代码编辑器：语法高亮、行号、自动缩进
- 项目管理：解决方案/项目树
- 编译集成：编译按钮、编译日志浏览
- 运行集成：运行/停止按钮、输出显示

**移植策略：**
- 推荐方案 B（LSP），支持 VS Code / JetBrains 等现代编辑器
- 保留 Qt 版本作为参考/备用

### 阶段 11：标准库与生态

- IO 库：控制台输入输出、文件读写
- 文件系统：路径操作、目录遍历
- 数学库：三角函数、随机数
- 字符串高级：格式化、分割、查找、替换
- 集合类型：Map/Dict
- 包管理器：模块依赖管理

---

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P0 | 8e-2/3/4. 集合（List/Dict） | 应用最频繁的数据结构；目前倾向于无泛型 + `as` 取值 |
| P2 | 9. 高级特性 | 按需实现，含异常、字符串插值、增量赋值、for-each 等 |
| P2 | 10. IDE 移植 / LSP | 开发效率；推荐 LSP 方案支持现代编辑器 |
| P3 | 11. 标准库 | 逐步完善 |

> 阶段 12（EN 引擎集成）已移除：NLang 作为独立语言演进，不再以与 EN 集成为目标。

## 当前状态

- 阶段 0-8e-1.5 已完成，**184 个 e2e 测试全部通过**
- 下一步：阶段 8e-2（泛型 `<T>`）→ 8e-3（`List<T>`）→ 8e-4（`Dict<K,V>`）

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/language-spec.md | NLang 语言规范（类型语义、语法、GC 行为） |
| docs/vm-architecture.md | VM 架构设计（编译管线、堆布局、GC 算法、指令集） |
