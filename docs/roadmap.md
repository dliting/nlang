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
- 字符串特例：`string.getHashCode()` 值哈希、`string.equals(string)` 值相等
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

### 阶段 8e-3：内置泛型 `List<T>` ✅
- 擦除式泛型（Java 模型）：T 仅编译期，运行时统一 Object 存储
- 仅内置（无用户定义 `class Foo<T>`），通过 ExprResolver 名称识别 "List"
- 文法扩展：Type 规则新增 `NameExpr '<' TypeList '>'`，构造识别 `new List<int>()`，`as` 接受 Type
- 新 AST 节点 SnGenericTypeExpr，持有 base NameExpr + 类型参数列表
- 每个 (baseName, typeArgs) 元组合成独立的 SnClassDecl，缓存避免重复（保持指针一致性不变量）
- 双签名模型：合成 SnClassDecl 携带代入后的类型检查签名；运行时调用擦除签名（原始类型参数退 Object）
- 9 个新内建（INTR_List_Ctor/Add/Get/Set/Length/RemoveAt/IndexOf/Contains/Clear）
- 运行时存储：m_listStore + m_listFreeList；List 实例通过 `__handle` 隐藏字段关联到 ListSlot
- 所有元素统一为堆 idx（原始类型在调用点 OP_Box 装箱）；GC MarkPhase 显式追踪 List 实例的元素，SweepPhase 回收 handle
- NewExpr codegen 修复槽位别名问题：构造参数先求值，然后在 callParamBase 之后分配 OP_New 结果槽，再复制到 resultOffset（避免参数槽覆盖）
- List 方法分派直接设置 m_pField（避免 ResolveFieldExprAs 用 SnType 覆盖 EvalDataType，破坏链式调用类型推导）
- 10 个新 e2e 测试（list_int_basic 到 list_as_field），共 194 个测试全部通过

### 阶段 8e-3 fix-up：List<T> 代码审查修复 ✅
- C1（严重）：boxing codegen 因 RTK_Int32==0 哨兵冲突从未 emit OP_Box；用 `bool needsBoxing` 标志修复
- C2（严重）：IndexOf/Contains 直接比较堆 idx；改为按 m_slotKinds 分支——RTK_Boxed 比较值位，RTK_Class 比较 idx
- H1：4 个 List 内建静默 no-op null-this；统一改为 throw（ReadListHandle 辅助函数）
- M1：增加 thisHeapIdx 上界检查（"stale reference" 防止悬挂引用 UB）
- GC UB：原元素为原始 int 时 m_slotKinds[elem] OOB；C1 修复后元素全部装箱，GC 安全
- S4/S6/S8 清理：删死代码 TraceList、提取 kListHandleFieldOffset/kBoxedValueSlot 常量、用 SnClassDecl::BaseName() 替代字符串 find('<') 手术
- 10 个新边界测试（list_empty_length 到 list_null_class_element），共 204 个测试全部通过

### 阶段 8e-4：内置泛型 `Dict<K,V>` ✅
- 复用 8e-3 擦除式泛型基础设施（合成 SnClassDecl 缓存、双签名、m_bIsGenericInst 标志、旁路存储表）
- ExprResolver 通用化：IsBuiltinGenericClassName 接受 "Dict"；GetGenericClassDecl 按 baseName 分派 arity（Dict=2，List=1）；显示名按 arity 逗号拼接
- 文法已支持 arity-N（TypeList 递归），AST SnGenericTypeExpr 持 vector — 无需修改
- 7 个新内建（INTR_Dict_Ctor/Set/Get/ContainsKey/Remove/Clear/Count）
- 运行时存储：m_dictStore + m_dictFreeList；Dict 实例通过 __handle 关联到 DictSlot{vector<pair<K,V>>}
- **线性扫描** lookup（O(n)），kind 感知的 DictKeysEqual 辅助函数（RTK_Class 比较 idx；RTK_Boxed 按内部 tag 分支：int/float 比较值位，string 比较 pool 内容）；hashtable 优化推迟到未来阶段
- codegen 重构：boxing 由 List 专用代码改为 per-method plan（argPlans map + returnsBoxed/returnTag），共享 BoxingTagFor 辅助函数（返回 {tag, isPrimitive}，避免 RTK_Int32==0 哨兵冲突）
- GC MarkPhase/SweepPhase 增加 Dict 分支，追踪每个 entry 的 K 和 V 堆 idx
- 10 个新 e2e 测试（dict_int_int_basic 到 dict_get_missing_throws），共 214 个测试全部通过

### 阶段 8e-5：`foreach` 语句 ✅
- 文法：新增 `KT_Foreach` / `KT_In` 关键字；`ForeachStmt: KT_Foreach '(' Type TT_Identifier KT_In Expression ')' Statement`，无 LALR 冲突（`KT_Foreach '('` 是唯一前缀）
- AST：`SnForeachStmt` 节点（VarType / VarName / Iterable / Body），X-macro 自动生成 `NK_ForeachStmt`
- **索引式展开**，不引入新 opcode。复用现有 `OP_ArrayLength`/`OP_LoadElement`（Array）、`OP_CallMethod "Length"`/`"Get"`（List/Dict-after-Keys）、`OP_Less_i32`/`OP_Add_i32`/`OP_JumpIfNot`/`OP_Jump`（循环）、`OP_Box`/`OP_Unbox`（per-method boxing plan）
- **三路 codegen 分派**（在 codegen 阶段，不在 resolver）：保留用户可见 AST，避免 diagnostics/source-mapping 异常
  - **Array** `T[N]`：iterSlot 类型 `RTK_Array`，长度 `OP_ArrayLength`，元素 `OP_LoadElement`（struct 元素加 `OP_CopyStruct`）
  - **List<T>**：iterSlot 类型 `RTK_Class`，长度 `OP_CallMethod "Length"`，元素 `OP_CallMethod "Get"` 后按 T 是否原始类型决定 `OP_Unbox`
  - **Dict<K,V>**：iterSlot 类型 `RTK_Class`，**inline `Keys()` 调用**（codegen 阶段，不修改 AST）物化 `List<K>` 到 iterSlot，之后与 List 路径完全相同（元素类型 = K）
- **隐藏局部变量 uniquification**：`AllocLocal` 按名 dedup，嵌套 foreach 会冲突；用 `FuncContext::foreachCounter`（每次函数入口重置）给 `__foreach_iter_<N>` / `__foreach_i_<N>` / `__foreach_n_<N>` 加后缀
- **`typeKind` 正确性**：iterSlot 的 `LocalDescriptor.typeKind` 必须匹配 iterable 类型（RTK_Array vs RTK_Class），否则 GC root tracing 会出错
- **`Dict.keys()` 内建方法**（`INTR_Dict_Keys = 60`）：返回全新 `List<K>` 堆实例，从 `dict.entries[i].first` 复制 keys。对 foreach 有用，独立使用也有用（key snapshot、set-style 成员检查）。返回的 List 是**拷贝**——后续 `Set`/`Remove` 不影响已返回的 List
- **`LoopContext` 复用**：`break`/`continue` 跨所有循环形式（for/while/do/foreach）走同一份逻辑，无需 foreach 专用代码
- 12 个新 e2e 测试（foreach_array_int 到 foreach_dict_keys_class），共 226 个测试全部通过
- **已知限制（P3.10 已修复）**：~~`List<int>` 包含字面值 0 的元素会触发 `OP_Box` 的 null-sentinel 优化路径~~ — P3.10 移除了 OP_Box 的 null-sentinel 优化（`if (val == 0 && typeTag != RTK_String) break;`），OP_Box 现在总是分配堆槽。Null literal 不走 OP_Box（走 TCK_Auto），所以 `Object o = null` 不受影响

### 阶段 8e-6：集合初始化器 ✅
- 文法：bare `[...]`（数组/List）+ 显式 `new Type{...}`（任意位置）；bare `{...}` 因与 CompoundStmt LALR 冲突被放弃
- AST：`SnInitListExpr`（携带 `ExplicitType` 字段、`InitEntry` 列表、`isArrayForm` 标志），X-macro 生成 `NK_InitListExpr`
- **类型推断**：resolver 接受 expected-type 参数（vardecl RHS / assignment RHS / return / 函数 arg），bare 形式从上下文取目标类型；显式形式以 `ExplicitType` 为准
- **5 路 codegen 分派**（VmBackend `NK_InitListExpr` handler）：
  - Array `T[N]`：`OP_AllocArray` + 每元素 `OP_StoreElement`
  - List<T>：`OP_New "List<T>"` + 每元素 `OP_CallMethod "Add"`（复用 8e-3 per-method boxing plan）
  - Dict<K,V>：`OP_New "Dict<K,V>"` + 每条目 `OP_CallMethod "Set"`
  - Struct/Class：`OP_New "Type"` (无参 ctor) + 每字段 `OP_StoreField`
- **递归嵌套**：每个 init list 从父取元素/值/字段类型，child init list 自动走同一 handler
- 19 个新 e2e 测试（init_array_int_basic 到 init_explicit_empty），共 245 个测试通过
- **8e-7 边界用例补充**（252 测试）：foreach over null（array/list/dict 抛错）、`foreach_empty_dict`、init list 含函数调用/单元素/负数字面量

### 阶段 8e-8：二元表达式对称类型提升 ✅
- **问题**：旧规则 `sn.EvalDataType(sn.Left()->EvalDataType())` 导致 `1 + 2.5`（int+float）= int（rhs 被截断）但 `2.5 + 1`（float+int）= float，左右不对称
- **修复**：改为 C-style 对称提升——两侧提升到 wider type：`int + int → int`、`int + float / float + int → float`、`float + float → float`、`string + string → string`（仅 OP_Add）
- **Resolver 改动**（`ExprResolver.cpp` `Access(SnBinaryExpr&)` else 分支）：计算 T_result 后，对每个 child 调用 `FixupExprType` 包装 `SnCastExpr`（若类型 != T_result）
- **Codegen 改动**（`VmBackend.cpp` binary 路径）：迭代 `bin.Children()` 而非 `Left()/Right()`（包装后 m_pLeft/m_pRight 失效）；dispatch 用 `leftChild.EvalDataType()`（包装后 = T_result；comparison 未包装时 = 操作数类型，用于选 OP_Eq_str 等）
- **FixupExprType bug 修复**：构造 SnCastExpr 后未设置 EvalDataType，导致 codegen 看到 null 类型；现在显式设为 castInfo.Target()
- 5 个新 e2e 测试（mixed_int_float_add / mixed_float_int_add_symmetric / mixed_int_float_mul / mixed_nested_promotion / mixed_assignment_chain），共 257 个测试通过
- **零回归**：现存 e2e 测试无 int+float 显式 mixed 用例，所有 252 个旧测试保持通过
- **为 Phase 9 铺路**：compound assignment（`x += y`）将直接继承新规则，无需特殊类型处理

### 阶段 8e-9-pre：命名约定统一 ✅
- **决策**：类型（class/struct/enum/interface）PascalCase；方法 / 自由函数 / 变量 camelCase；`main` 唯一例外；`getHashCode` 保留 Get 前缀（为未来 property 特性保留 `getXxx`/`setXxx` 命名空间）
- **rationale**：`MyClass.myMethod()` 视觉上立即区分类型与方法（vs `MyClass.MyMethod()` 歧义）；覆盖 Java + JS + C++ 开发者群体；`main` 沿用 C/C++/Java 入口惯例
- **迁移范围**：~33 个内置方法重命名（`Length→length`、`Add→add`、`Equals→equals`、`GetHashCode→getHashCode`、`ReadInt→readInt`、`Keys→keys`、`ContainsKey→containsKey` 等）
- **src 改动**：`VmBackend.cpp`（~30 处方法名 literal + AddStringConstant 调用）、`VmExecutor.cpp`（~40 处错误消息字符串）、`ExprResolver.cpp`（~15 处 `name == "Xxx"` 比较）
- **tests 改动**：89 个 `.n` 文件批量更新调用点（Python 脚本，正则 `\.<OldName>\(` → `.<newName](`，外加声明处的 `\b<OldName>\(`）
- **docs 改动**：`language-spec.md` 新增"Naming Convention"章节；3 个 md 文件同步示例代码
- 257 个测试零回归（test e2e 通过率不变）
- 详见 memory: `nlang-naming-convention.md`

### 阶段 8e-9a：基元 → string 自动强制转换 ✅
- **问题**：`int + string` 之前因 SnCastExpr codegen（`VmBackend.cpp:951`）只处理 `int↔float`，对 `int/float → string` 静默 no-op（虽然 `CastInfo.cpp:13,18` 已 `TCK_Auto` 允许），导致 int 位模式被 OP_Concat_str 误读为字符串 idx
- **8e-8 临时的 strengthening**（reject `non-string + string`）已撤销
- **设计**：仿 `OP_CastIntToFloat` 模式新增 `OP_Int32_to_str` / `OP_Float_to_str` 两个 opcode（implicit pResult，无 immediate operand，后跟 OP_Assign）
- **格式化**：int 用 `std::to_string`（decimal），float 用 `%g`（`2.5` → "2.5" 而非 "2.500000"，对齐 Python `str(2.5)`）
- **Codegen 改动**（`VmBackend.cpp:951-967`）：SnCastExpr dispatch 增加 `int→string` / `float→string` 两个分支
- **Executor 改动**（`VmExecutor.cpp:~190`）：紧跟 `OP_CastFloatToInt` 新增两个 case，把格式化字符串 push 到 `m_stringPool`，写回新 idx 到 pResult
- **Resolver 改动**（`ExprResolver.cpp:758-769`）：删除"非全 string reject"段，保留"非 Add on string reject"；`int + string` 通过 FixupExprType 包装非-string 操作数为 SnCastExpr（TCK_Auto）
- **Disassembler**：`OpCodeTable.cpp` 新增 `"int32_to_str"` / `"float_to_str"` 字符串映射；`ndisasm/main.cpp` switch 加入两个 case（no-operand 组）
- 6 个新 e2e 测试（`string_concat_int_right`、`_int_left`、`_float`、`_chain`、`_int_assign`、`_negative_int`）
- **遗留**（Phase 8e-9b 已解决）：class → string 已实现（Object.toString() 协议）；enum → string 输出枚举名（OP_Enum_to_str）
- 详见 memory: `nlang-phase-8e-9a-primitive-to-string-design.md`

### 阶段 8e-9b：Object.toString() 协议 ✅
- **设计**：Object 基类新增 `string toString()` virtual 方法（默认 `"ClassName@hex(heapIdx)"`），用户 class 可 override
- **Enum**：新增 `OP_Enum_to_str <enumDefIdx>` opcode，编译期嵌入 `enumNames` 名表，运行时查表输出枚举名（"Red" 而非 "0"）
- **String**：`string.toString()` 是 identity（resolver 折叠，无 opcode）
- **Int/Float**：复用 8e-9a 的 `OP_Int32_to_str`/`OP_Float_to_str`
- **隐式 coercion**：`"x" + obj` 自动调用 `obj.toString()`（扩展 8e-9a strengthening）
- **架构**：resolver 只设 `EvalDataType=String + NF_Resolved`，codegen 基于 `outer->EvalDataType()` 分派（不用 m_pField 侧通道）
- **Struct 永久排除**：`struct.toString()` / `"x" + structInstance` 永久编译错误
- **Intrinsic**：`INTR_Object_toString = 44`，null receiver 抛 NPE
- **模块序列化**：版本 1.2，新增 `enumNames` 字段
- 10 个新 e2e 测试 + `string_concat_enum` 期望值更新（2→4）
- **P3.9 8e-9b 边界测试**（311-313）：`enum_tostring_out_of_range_throws`（enum value 越界抛 runtime error）、`class_tostring_inherited_override`（继承链 virtual dispatch 走 Base override）、`class_tostring_object_ref_override`（Object 引用调用 override toString 而非 intrinsic）
- **P3.10 OP_Box null-sentinel bug 修复**：`VmExecutor.cpp` OP_Box 的 `if (val == 0 && typeTag != RTK_String) break;` 优化错误地将 int 0 和 float 0.0 视为 null sentinel，导致 `List<int>.add(0)` / `List<float>.add(0.0)` 存入 heapIdx=0，后续 `get()` + OP_Unbox 抛 "unbox on null/invalid reference"。修复：移除 null-sentinel 优化，OP_Box 总是分配堆槽。Null literal 不走 OP_Box（走 TCK_Auto），所以 `Object o = null` 不受影响。`list_int_zero_throws` / `list_float_zero_throws` 重命名为 `list_int_zero` / `list_float_zero`，期望值从 1 改为 7
- 详见 memory: `nlang-phase-8e-9b-tostring-design.md`

### P3 loop refinement（2026-08-08）✅
- **P3.2 OP_CallIntrinsic bug 修复**（commit 862d7a7）：`VmExecutor.cpp:744` `OP_CallIntrinsic` case 一直 throw "intrinsic calls not yet implemented"，但 `VmBackend.cpp:1337/1356` 为 `string.getHashCode()` / `string.equals()` emit 此 opcode（strings 是 primitive，无法走 `OP_CallMethod callee.intrinsicId` 路径）。测试 `string_gethashcode.n` / `string_equals.n` "通过"纯属巧合——VM throw → exit 1，恰好等于 manifest 期望 1。修复后用 `ExecuteIntrinsic` 分派；测试成功退出码改为 7 防回归
- **P3.3 8e-9a 边界测试**：新增 4 个测试覆盖原 6 个的盲区——`int+float+str` 提升顺序（验证 int 先升 float 再转 string，不是 "x12.5"）、负 float（`%g` 保留符号）、float 在左（对称性）、float 赋值路径（不经过 binary）。共 267 个测试通过
- **TODO 扫描**（6 处）：`ModuleBuilder.cpp:216` ResolveDataValues（placeholder）、`ScriptScanner.cpp:32` fopen portability、`SyntaxNode.cpp:185` AllowProtectedAccess（OOP protected 语义未实现，无测试）、`Module.cpp:117` LoadFrom（stream 加载未实现，Phase 11 包管理）、`Node.cpp:72` find 优化（micro-perf）、`VmExecutor.cpp:615` UTF-8 code points（Phase 9+ 特性）。全部 forward-looking，无 bug
- **P3.6 测试覆盖补充**（275-281）：`mixed_int_float_sub/div`（关闭二元提升覆盖缺口）、`string_concat_int_max/min`（int32 边界）、`list_int_zero_throws`（锁定 List<int> null-sentinel bug）、`foreach_dict_int_int`、`foreach_list_float`、`gc_stress_over_threshold`（首次跨过 GC_THRESHOLD=1024 触发 MarkPhase+SweepPhase）、`class_method_chain`、`arithmetic_mod_negative`（锁定 % 的 C 截断符号约定）
- **P3.7 编译器 segfault 修复**（commit 04625a4）：`ParseSources` 把 `ParseTransUnits → MergeTransUnits` 串成无门控链；顶层语法错误时 `CompileUnit` 规则不归约，`TranslationUnit::m_pRoot` 留 null，`MergeFrom` 解引用 null → exit 139。修复：`ParseTransUnits` 后加 `if (HasError()) return false;` 早退。同一提交还修了 `ScriptParser::ParseUnit` 反向条件 `yyparse==0 && HasError()` → `!HasError()`（返回值当前被忽略，逻辑修订防未来踩雷）
- **P3.7 缺失输入文件 segfault 修复**（commit c3152cc）：`ScriptScanner::OpenFile` 用裸 `LogError`（fprintf 到 stderr），不增 `BuildEnvironment` 错误计数；`HasError()` 早退不触发。修复：`ParseUnit` 在 `OpenFile` 失败时用 `env.Log(CLL_Error,...)` 再记一次
- **P3.7 锁定整型溢出行为**（commit ee32291）：`int_overflow_wrap.n` 验证 C 风格补码 wrap（INT_MAX+1 → INT_MIN），无 SafeInt 抛错
- **P3.7 锁定浮点除零行为**（commit f584522）：`float_div_zero_throws.n` 验证 NLang 浮点除零抛错（不走 IEEE 754 ±inf）
- **P3.7 nvm 模块加载硬化**（commit 2404bb4）：malformed `.nmod` 文件导致 nvm 抛 `std::runtime_error` 但 `Load()` 在 try/catch 之外，引发 `std::terminate` → exit 3（abort）。修复：`Load()` 也并入 try/catch；`ModuleLoader::Load` 加 `fs.good()` 检查 + 16MiB 上限，区分 truncation / bad-size / bad-magic 三种错误
- **P3.8 interface 单行声明调查结案**（commit 6be6b14）：非 bug——interface 方法默认 `FA_Private`（NLang 默认访问修饰符），无 `public` 时不可访问；错误信息"does not exist"虽不准确（实际是 inaccessible）但语义合规。文档化到 `language-spec.md`（新增 Interface 章节）
- **P3.8 测试覆盖补充**（284-293）：`list_float_zero_throws`（锁定 List<float> 0.0 null-sentinel bug，P3.1 范围扩大）、`list_struct_basic`（List<struct> boxing 往返）、`class_field_defaults`（int/float/string/class-ref 默认值）、`switch_string`（字符串 switch 分派）、`struct_nested_default`（嵌套 struct 递归物化）、`virtual_deep_dispatch`（4 层继承虚分派）、`class_implicit_upcast`（Dog→Animal 隐式上转）、`gc_stress_array`（数组 GC 阈值跨越）、`func_implicit_return`（无显式 return 默认 0）、`string_concat_mixed_chain`（8e-8 提升 + 8e-9a coercion 在同一表达式中组合）
- **P3.8 锁定 switch 无 fall-through**（commit d2b8945）：`switch_no_fallthrough.n` 验证 NLang switch 无 C-style fall-through——每个 case body 隐式 break（Java/C# 语义，非 C/C++）。codegen 实现为 per-case 等值检查：body 执行后控制流进入下一个 case 的比较，因 value 不再匹配而跳过其 body。同时修正 `language-spec.md` 错误描述（commit 20af144）



---


## 后续阶段

### 阶段 9：高级语言特性

分批实施（9a-9f）：

**9a：增量赋值 + 断言 + const** ✅（328 个 e2e 测试通过）
- 增量赋值：`x += 1;`、`x -= 1;`、`x *= 2;`、`x /= 2;`、`x %= 3;`
  - 支持 local 变量、class/struct field（左值单次求值）
  - 不支持 subscript `arr[i] += 1`（4 scratch slot 限制，用户写 `arr[i] = arr[i] + 1`）
- 断言：`assert(condition);`（失败抛 OP_AssertFail → main 捕获 → exit(1)）
- 常量修饰：`const int X = 5;`（仅局部 const，声明必须初始化，assign/compound-assign 编译错误）

**9b：字符串插值（MVP — `${identifier}` only）**
- `"Hello ${name}"`：仅支持单个标识符插值（不支持复杂表达式）
- `$$` 转义为字面 `$`
- 无效 `${...}` 内容、未定义标识符均报编译错误
- 实现：bison `TT_String` 规则扫描字符串内容，构造 `OP_Add` 二元树，复用 Phase 8e-9a 对称 coercion + Phase 9b-pre collection toString
- 357 e2e 测试通过（含 13 个新 interp_* 测试）

**9c：默认参数 + 命名参数** ✅（393 个 e2e 测试通过）
- 默认参数：`int foo(int x, int y = 0)` — 任意位置（不限于末尾），默认表达式可引用前面的形参（`int b = a + 1`）
- 命名参数：`foo(b = 2, a = 1)` — 位置参数在前，命名参数在后
- 重载集成：多候选评分 + 歧义检测（`foo(int a)` 与 `foo(int a, int b = 0)` 对 `foo(5)` 歧义报错）
- 架构：共享 AST + 调用点 binding override（OverrideScope RAII 栈），零 AST 克隆，零新 opcode
- 默认表达式引用 `this.field`：m_ThisOverrideStack + resultOffset（避开 callParamBase 嵌套覆盖陷阱）
- callParamBase 8 槽上限：声明期编译错误（kMaxFreeFuncParams=8 / kMaxMethodParams=7），不再静默腐败
- 前向引用检测：默认表达式不能引用后置形参（`int foo(int a = b, int b = 5)` → 编译错误）
- 4 轮深度审计（round 3-7），36 个新 e2e 测试（含 11 个 compile_error 测试）

**9d：异常处理** ✅（442 个 e2e 测试通过）
- try/catch/throw（Java/C# 风格 class 层级，全 unchecked）
- Built-in Exception + NullPointerException + DivByZeroException + IndexOutOfBoundsException + AssertionException
- 多 catch 子句（按声明顺序匹配第一个）、rethrow（`throw;`）
- VM 错误可 catch：NPE/除零/OOB/assert 包装为 Exception 子类实例
- Zero-cost try：per-function tryBlocks 表（throw 时线性扫描）
- NLangThrow : public std::runtime_error（未捕获异常冒泡到顶层 exit 1）
- Per-CallFrame handlerExcStack（非 executor 全局，避免跨函数残留）
- 3 新 opcode：OP_Throw / OP_Rethrow / OP_PopHandler
- 模块格式 v1.4（不向下兼容 v1.3）
- Exception 字段暴露：`e.message`（string）/ `e.backtrace`（List<string>）可读写，用户子类正确继承（flattened layout：slot[1]=message, slot[2]=backtrace, slot[3+]=own fields）
- break/continue 退出 catch 体时正确平衡 handlerExcStack（按 loop entry depth 差值 emit OP_PopHandler）
- 30 个新 e2e 测试（23 初始 + 7 follow-up）

**9d-2：finally 块 + super() 构造器链** ✅（461 个 e2e 测试通过）
- finally 完整 Java 语义：try 正常完成 / catch 完成 / 异常 unwinding / break / continue / return 全部先执行 finally
- 纯 codegen trampoline 实现，零运行时改动：catch-all tryBlocks entry（exceptionClassIdx=0xFFFF，表尾 push，first-match 扫描下 typed catch 优先）+ finallyHandler（body 副本 + OP_Rethrow）+ finallyNormal（body 副本）
- 关键设计：finally entry 的 rangeEnd 覆盖 catch handlers（catch body 内 throw 也过 finally）；所有 finally 副本在覆盖范围之外（无双执行）；`$finally_exc` 隐藏 scratch local（运行时 handler 入口无条件写 catchLocalOff）
- break/continue/return trampoline：内联 finally body 副本（innermost → outermost）后再跳转/返回；return expr 求值先于 finally（值语义正确）
- `super(args);` 构造器链：任意语句位置（不强制首语句）；复用 NewExpr ctor 发射模式（EvalAreaClaim + bulk copy + OP_CallMethodDirect）；支持 built-in Exception 家族父类（super(msg) 转发 message）
- Resolver 验证：super() 仅 ctor 内、arity 匹配父 ctor、拒绝命名参数、父类无 ctor + 传参报错（无 ctor + 零参 = 合法 no-op）
- 限制：finally body 内禁止 break/continue/return/throw（防止吞控制流/吞异常语义）
- 20 个新 e2e 测试（11 finally 路径 + 1 compile_error + 8 super，其中 4 compile_error）；exception_super_ctor.n 改为真实 super(msg) 语义；删 exception_finally_not_supported

**9d-2 follow-up：裸字段访问（implicit this.field）** ✅（468 个 e2e 测试通过）
- Bug：方法/ctor 内裸标识符（`v = x; return v; v += 1;`，v 为类字段）在 codegen 走 FindLocal 抛异常，且异常逃出 ncc main → 未处理 MSVC C++ 异常 → exit 3 静默崩溃（buffered 输出丢失）
- 架构修复：ResolveBareIdentifier 成为 codegen 侧唯一绑定决策点（local frame → implicit this.<classField> → NotFound），与 resolver 绑定顺序镜像，三个消费点（identifier 读 / AssignStmt 写 / CompoundAssignStmt）共享；OwningClassOfMemberField 经 field->Parent() 取 owning class（继承字段取正确 flattened offset）；ImplicitThisSlot() 区分方法体（local 0）与默认参数 caller 上下文（this-override slot）
- 边界修复：ncc main 的 builder.Build() 包 try/catch——未来 codegen 内部错误打印 "Compiler internal error: ..." + exit 1，不再静默 abort
- 7 个新 e2e 测试（read/write/ctor-init/inherited/compound/local-shadow/default-param-field）
- 长期方向（记录，暂缓）：在 resolver 完成后做一次 AST normalization pass（裸字段 → 显式 ThisExpr/MemberExpr），可消除 resolver/codegen 双模型漂移；因现有 visitor 会 mutate AST（默认参数改写等），引入该 pass 有风险，待未来重构窗口

**9d-3：array-of-struct 物化修复 + GC 根集修复** ✅（474 个 e2e 测试通过）
- Bug：`Point[] arr; arr[0].x = 1` 抛 "struct field store out of bounds"——AllocArrayOnHeap 将元素零初始化而非物化 struct 实例。调试中又暴露 3 个互锁 codegen bug 与 1 个 GC 根本缺陷，共 5 项修复：
- 物化：AllocArrayOnHeap 对 struct 元素循环 AllocStructOnHeap（每元素独立实例，值语义；与 AllocStructOnHeap 递归物化嵌套字段同语义）；零长度 struct 数组合法（循环体不执行）
- IsArrayType() 守卫（2 处）：局部赋值 + struct member 写路径。EvalDataType() 对数组类型表达式返回**元素类型**，struct 深拷贝分支会把整个数组 block 误当 struct 拷贝（破坏 kind 元数据 + GC 追踪）。架构不变量：**一切按 EvalDataType()->Kind() dispatch 的消费点必须先查 IsArrayType()**（与 8e-9b toString 注释同源）
- array.length dispatch hoist：struct 字段 dispatch 遮蔽了 `Point[] b; b.length`（FindFieldOffset 返回 -1 后静默 return，只发射 receiver）→ array.length 检查移到 struct/class dispatch 之前
- 下标读去掉防御性 CopyStruct + member 赋值求值顺序改 receiver-first（Java JLS 15.26.1）：值拷贝只发生在赋值/存储边界（VM 不量）；原先读路径副本使 `arr[0].a.x = v` 写进废弃副本，且 RHS→tempSlot2 先求值时下标索引 scratch 槽（PickTempSlot(tempSlot)=tempSlot2）反噬 RHS
- **GC 根集根本修复（模块格式 v1.5）**：.nmod 从未序列化 func.locals → MarkPhase 根扫描在运行时永远空集 → 任何真实 collection 会清扫全部活对象。既有 GC 测试全部通过纯属巧合（无活对象跨 collection，或分配数未过 1024 阈值）。v1.5 增加 local-variable descriptors（offset/size/isParam/typeKind/name）；旧格式模块根集为空（向后兼容读）
- 7 个新 e2e 测试（array_struct 系列：basic/zero_default/distinct/nested/foreach/gc/empty）

**9d-3 audit：数组字段 + struct 数组边界** ✅（479 个 e2e 测试通过）
- 复审发现 9d-3 的 receiver-first 求值顺序重排引入回归：`b.a = new int[2]`（struct/class 数组字段赋值）中 NewArrayExpr 的 size 操作数硬编码 tempSlot → 覆写 receiver。修复：size scratch 改 PickTempSlot(resultOffset)（与 InitListExpr 同一纪律——**内部 scratch 必须避开自己的 dst**）
- MarkPhase 补数组字段追踪：struct/class 的数组字段在 fieldTypeKinds 记录为元素类型（已知误分类），静态 kind 无法识别 → 按 m_slotKinds[refIdx]==RTK_Array 运行时 kind 追踪（最坏 over-retention，对 mark-sweep 安全）
- 实证澄清：foreach over struct 数组对元素深拷贝（OP_CopyStruct 到循环变量）——9d-3 最初记录的"别名"遗留是错的，已从文档移除；struct 数组作参数（`f(Point[] p)`）本来就正确
- 5 个新 e2e 测试（array_struct_param/foreach_struct_copy/field_array_struct/field_array_struct_gc/field_array_class_gc）
- 已知遗留：`matrix[i][0].x = v`（嵌套下标 receiver）的 RHS 碰撞未修；eager 物化的分配成本（数组重设计时再议）；fieldTypeKinds 数组字段误分类的彻底修复（RTK_Array 记录 + 物化/深拷贝/序列化消费点同步）留数组重设计

**小修复：pResult 累加器过期（string pool dedup bug）** ✅（484 个 e2e 测试通过）
- Bug：`"s" + (a+b)` 产生正确长度字符串但 `==` 失败——OP_Int32_to_str 等转换 opcode 读 pResult 累加器，但 EmitExpression 只有当源表达式最终 opcode 写累加器（var_local/consts/call）时才把值留在 pResult；写 locals 的源（二元算术 `add_i32`、字段/元素 load）留下过期 pResult
- 同根因家族：cast_f2i quirk（float 字段直接转 int 需中间局部 workaround）、`(a+b).toString()` 乱码、`list.add(a+b)` 装箱垃圾值
- 修复：EmitPResultRefresh（发 OP_VarLocal 从保证有值的槽重载 pResult）插入 12 个读累加器的位点（OP_Box/OP_Unbox/OP_CastIntToFloat/OP_CastFloatToInt/OP_Int32_to_str/OP_Float_to_str 前）；3 处 pResult 新鲜的位点（CallMethod/ConstString 后的 Unbox/Box）不动
- 5 个新 e2e 测试（string_concat_inline_arith/tostring_inline_arith/list_add_inline_arith/float_cast_inline_arith/cast_float_field_to_int）
- 架构不变量：**读 pResult 累加器的 opcode 之前必须保证 pResult 新鲜—— EmitExpression 不承诺累加器语义，只承诺值在 dst 槽**

**小修复：两处 frame 布局越界（ASan 扫描发现，exception_super_ctor flake 根治）** ✅（485 个 e2e 测试通过，普通 + ASan 双构建零报告）
- exception_super_ctor 全量套件 ~0.5% STATUS_HEAP_CORRUPTION：MSVC ASan 构建（`-fsanitize=address`，独立 build-asan 目录）使其 100% 确定性复现，暴露两处越界：
- **super() 参数 staging 欠尺寸 evalArea**：peakDepth walker 的 SuperCallStmt 分支漏了 codegen 发射的 EvalAreaClaim(1+args)（MaxArgsWalker 有——Phase 9c 的 codegen-walker 对称纪律在 9d-2 落地 super() 时未应用，同类第 3 例）。staging 每次写越界一槽，~0.5% 堆布局恰好撞元数据才崩溃。修复：StmtPeakDepth 补 claimSize
- **`new C{...}`（类有参 ctor）传垃圾参数**：规范要求类 init 必须无参 ctor（lower 为 new C() + 逐字段 store），但 resolver 从未强制——codegen 照样调 ctor，OP_CallMethodDirect 从欠尺寸 callParamBase 拷贝 ctor.paramCount 槽 = 越界读 + 垃圾进 ctor。修复：ExprResolver Access(SnInitListExpr) 强制约束；4 个靠"垃圾被字段 store 覆盖"侥幸通过的测试改为规范形式 + 1 个 compile_error 测试
- 教训：**GC/堆正确性类 flake 直接上 ASan，不要做统计推断**（0/80 vs 1/260 的对比浪费了一小时，ASan 一次运行就给出确定性答案 + 符号化栈）

**小修复：条件类型强制 int** ✅（488 个 e2e 测试通过）
- OP_JumpIfNot 只读单个 int32：string 条件是 pool handle（索引 0 编码为 0 → 非空字符串可误判 false）、struct/array 是 heap 索引、float 靠 IEEE 位运（非零位≠0）。resolver 现在在 5 个条件位点（if/while/do-while/for/assert）强制 int（比较表达式本就产生 int）
- CheckIntCondition 共享助手（StatementResolveAccessor）+ 3 个 compile_error 测试；language-spec 补"Condition typing"节

**小修复：字符串转义补全** ✅（492 个 e2e 测试通过）
- lexer 只处理 `\n \r \t \"`；其余（含 `\\` 自身——源码 `"a\\b"` 产 4 个字面字符）静默按反斜杠字面量通过。补 `\\ \' \0 \a \b \f \v` + 未知转义改 compile error（不再静默通过）；language-spec 补 escape 表；插值与转义组合已验证（escape 在 lex 期应用，插值在 parse 期扫已转义内容）

**小修复：`>>` 拆分支持嵌套泛型** ✅（497 个 e2e 测试通过）
- `List<List<int>>` 被词法 `">>"` → OT_RSH 阻塞。调研发现 `>>` 从未有 grammar 产生式（%token/%left 声明了但无规则）——即位移运算符从未实现，`>>` 唯一的存在意义就是挡住泛型。采用 C# 风格 scanner 拆分：ScriptScanner 记 genericDepth（`<` 紧跟 List/Dict 标识符开一层，`>` 关一层，floor 0），depth>0 时 `>>` 经 yyless(1) 拆成两个 `'>'`（列号补偿 yycolumn -= n-1；`>>>` 经重匹配循环自动关三层——grammar 的 `Type: NameExpr '<' TypeList '>'` + `TypeList: Type` 本就递归支持，resolver 的 SnGenericTypeExpr 也递归解析 type args，唯一缺口就是词法）
- 已知限制：变量 shadow 类型名后紧接 `<` 比较（`List < 3`，中间仅空白/注释）会被误读为泛型开括号；depth-0 `>>` 仍为 OT_RSH（compile_error 测试锁定）；`<<` 同样无产生式
- 5 新测试：nested_generic_list/dict/deep（三层 `>>>`）、rshift_not_generic（compile_error 锁 depth 不泄漏）、angle_bracket_compare（比较回归）

**9e：out 参数**
- `void foo(int x, out int y)`

**9f：原生函数绑定**
- `native void foo();`

### 阶段 10：IDE 移植 / LSP

推荐 LSP 方案支持 VS Code / JetBrains 等现代编辑器。

### 阶段 11：标准库与生态

- IO 库：控制台输入输出、文件读写
- 文件系统：路径操作、目录遍历
- 数学库：三角函数、随机数
- 字符串高级：格式化、分割、查找、替换
- 包管理器：模块依赖管理

### 远期特性

- 代理/回调：函数指针、委托类型
- 用户定义泛型：`class Foo<T>`

---

## 实施优先级

| 优先级 | 阶段 | 说明 |
|--------|------|------|
| P2 | 9. 高级特性 | 分批实施 9a-9f |
| P2 | 10. IDE 移植 / LSP | 开发效率；推荐 LSP 方案支持现代编辑器 |
| P3 | 11. 标准库 | 逐步完善 |

> 阶段 12（EN 引擎集成）已移除：NLang 作为独立语言演进，不再以与 EN 集成为目标。

## 当前状态

- 阶段 0-9d-3（含 audit）+ pResult 累加器修复 + frame 布局越界修复（ASan 扫描）已完成，**497 个 e2e 测试全部通过**（`>>` 拆分支持嵌套泛型——C# 风格 lexer genericDepth + yyless(1) 拆分 + 5 新测试；字符串转义补全——`\\ \' \0 \a \b \f \v` + 未知转义 compile error + 4 新测试；条件类型强制 int 5 位点 + 3 compile_error 测试；frame 越界——super() evalArea 欠尺寸 + `new C{...}` 参 ctor 垃圾参数，resolver 强制规范约束；pResult 累加器过期修复——string pool dedup bug + cast_f2i quirk 同根因，12 位点 EmitPResultRefresh + 5 新测试；Phase 9d-3 audit 数组字段 + NewArrayExpr scratch + MarkPhase 数组字段追踪 + 5 新测试；Phase 9d-3 array-of-struct 物化 + IsArrayType 守卫 + array.length hoist + 值拷贝边界 + GC 根集 v1.5 + 7 新测试；Phase 9d-2 follow-up 裸字段访问 implicit this.field + 编译器异常边界 + 7 新测试；Phase 9d-2 finally 完整 Java 语义 + super() 构造器链 + 20 新测试；Phase 9d 异常处理 try/catch/throw + 5 个 built-in Exception 子类 + 字段暴露 + break/continue handler 修复 + 30 新测试；Phase 9c 默认参数+命名参数 + follow-up frame layout 重构 + 完整审计 + 跨模块导入基础设施 + Option B 跨模块默认参数；Phase 9b 字符串插值；Phase 9a 增量赋值/assert/const；以及之前所有阶段）
- Phase 9c follow-up（2026-08-11）：callParamBase 动态分配（8 槽 cap 解除 → 64 参数 sanity ceiling）；cursor-based evalArea + EvalAreaClaim RAII（嵌套调用 clobber 修复）；所有 bypass EmitCallArgs 的直接写路径（构造器参数、String.Equals/GetHashCode、Dict 初始化）已统一改造为 EvalAreaClaim 模式；walker 与 codegen 对称性已校验
- Phase 9c 跨模块导入（2026-08-12/13）：`import "X";` 语法 + CompiledModuleNodeBuilder（直接消费 CompiledModule，绕过 legacy RnFunction 管线）+ 两阶段 MergeImportedModules（Phase A: classes/structs/arrays；Phase B: functions + RemapBytecode）+ ModuleLoader v1.3 版本 + Option B 跨模块默认参数（仅 constant-foldable：literal/null/negative int fold；非 foldable 在 consumer 侧 compile_error）
- 8e-6 已知遗留（不影响测试通过）：bare `[]` 空 init（OT_Brackets 词法冲突）、
  nested generics `>>` 词法冲突、bare init list 作为函数参数（Phase G
  overload 唯一性检查未实现，可用 `new Type{...}` 显式形式绕过）
- 已知遗留（Phase 9b 发现）：`"s" + (a+b)` 字符串与内联算术表达式拼接后 `==` 比较失败（pre-existing string pool dedup bug，临时绕过：用单独变量 `int c = a+b;` 再 concat）
- 下一步：Phase 9e（out 参数）或小修复（string pool dedup bug、JumpIfNot string 条件误读、`\` 转义缺失、`>>` 解析）

## 文档索引

| 文档 | 说明 |
|------|------|
| docs/language-spec.md | NLang 语言规范（类型语义、语法、GC 行为） |
| docs/vm-architecture.md | VM 架构设计（编译管线、堆布局、GC 算法、指令集） |
