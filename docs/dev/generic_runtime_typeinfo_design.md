# v0.13.1 运行时泛型类型参数信息

- **文档定位**：v0.13.1的目标与范围界定，概要设计的输入。内容自roadmap条目
  拆出（用户指示2026-11-01原排0.12.8，网络库占用0.12.8后于2026-11-07改排
  0.13.1），并补入2026-11-07取证确认的四项设计与实现缺口——
  `generics_design.md`§三/§四所述机制中，源码未接线的部分。取证基于dev树
  dc6aa249，master树ab9bfb46复核，行号以master为准。
- **设计原则**（用户指示2026-11-01）：设计此功能时，应全面考虑基本的运行时
  类型信息和辅助设施的支持。

## 一、现状：字典模型已落地部分

- 编译期铸造与去重：实例缓存键=（模板节点，类型实参节点）指针恒等，同实参
  列表命中同一实例节点（src/compiler/GenericInstanceRegistry.cpp:86）；两
  阶段构造（壳先发布进缓存再填成员）使`Node<T>`自引用在深度1绑定
  （src/compiler/builder/ExprResolverGenericMint.cpp:261）；嵌套实例化深度
  守护16（src/compiler/builder/ExprResolver.h:259）。
- 注册拓扑：模板注册自身类行（裸名擦除键，src/vm/backend/Register.cpp:116），
  实例化是编译期类型、从不注册，全部实例化共用一个类行与一份编译记录
  （src/vm/backend/RegisterClass.cpp:290）；同一次构建跨单元经导入槽同骑
  模板行，不重解析不重编译（src/vm/backend/Slots.cpp:188-191；
  tests/e2e/proj_generic_cross_unit）。
- T槽表示：声明类型为类型参数的槽（形参/字段/静态槽/帧带局部）一律分类为
  `RTK_TypeParam`（include/nlang/vm/CompiledModuleRecords.h:35），GC按保守
  双域值测试扫描（src/vm/VmExecutorGCRoots.cpp:41,140；
  src/vm/VmExecutorGC.cpp:354；src/vm/backend/GenerateUnits.cpp:238）。
- TypeDesc wire：内建容器实例化按裸名+实参表写复合种类（深度上限8降级为
  `RTK_NonSerialized`），用户泛型实例化降为普通类行、类型实参不进wire
  （src/vm/TypeDesc.cpp:315-331,363-366）。
- 描述符设施：`GenericInstDescriptor`（规范拼写+进程级驻留池，拼写相等即
  描述符相等，src/vm/GenericInstDescriptor.cpp:71）已在树，仅单元测试引用，
  无生产调用方。

## 二、设计与实现的四项缺口（取证2026-11-07）

1. **描述符双载体未接线**。`generics_design.md`§三决定实例化描述符两路携带：
   对象头部存一份、静态可知的调用点作隐藏实参传一份（Go字典路线）。现状
   对象头首格仅类行索引（src/vm/VmExecutorOpsObjects.cpp:217），帧内无描述
   符槽、无隐藏实参通道。
2. **转型区分不了实例化**。设计决定转型按描述符递归比较。现状`OP_CheckCast`
   仅类行恒等+超类链行走（src/vm/VmExecutorOpsObjects.cpp:222-234），
   `Box<int>`与`Box<string>`共用裸名类行、运行时不可区分；编译期转型表同基
   （src/compiler/CastInfo.cpp:198-201,225-234）。
3. **catch未拒绝泛型类型**。设计决定catch类型位的泛型实例化编译期拒绝。现状
   门仅查Exception子类（src/compiler/builder/StatementResolverSwitchTry.cpp:
   348-352），`catch (Box<int> e)`可编译可运行；因运行时派发与`as`同基
   （src/vm/VmExecutorOpsObjects.cpp:284-291），实际语义是按模板名接住、
   所有实例化一起接——拼写承诺多于实际作为，拒绝比默许诚实。
4. **预编译包不带模板信息**。设计决定`.ncu`携带模板表与类型参数名表，消费方
   免源码铸新实例化。现状保存/加载代码对泛型零命中（src/vm/ModuleSaver.cpp、
   src/vm/ModuleLoader.cpp），`CompiledClass`记录无泛型字段
   （include/nlang/vm/CompiledModuleRecords.h:245-302）；导入桩签名重建无
   `RTK_TypeParam`分支、T叶子降int32占位，类型参数名不恢复，桩上
   `IsGenericDeclaration()`为假
   （src/compiler/builder/CompiledModuleStubSupport.hpp:186）。标准库现靠
   工具链随`.npkg`附`.n`源码，缺口未暴露。

## 三、v0.13.1目标

1. **描述符接线**（其余目标的先件）：对象头部携带实例化描述符＋静态可知的
   调用点隐藏实参；宿主ABI v11→v12（v11已被网络库占用，
   include/nlang/vm/NativeHost.h:71）。
2. **`typeName(类型表达式)`**：具体类型编译期折叠为字面量；类型参数`T`在
   共享方法体内读运行时描述符，返回规范拼写string（用户裁定）。用途=泛型类
   被禁静态字段的用户层补偿——非泛型类`static Dict<string,...>`按
   `typeName(T)`分键实现按实例化状态（C#`typeof`+`Dictionary<Type,...>`
   同款配套）；补偿模式随本版写入语言规格与用户手册（用户指示2026-11-01）。
3. **转型区分**：`as`与类型测试按描述符递归比较，`Box<int>`实例不可转
   `Box<string>`；编译期转型表与运行时执行基对齐。
4. **catch拒绝**：catch类型位的泛型实例化报编译期诊断；诊断文案与手册同步。
5. **`.ncu`模板表**：预编译包携带模板标记、类型参数名表与参数化签名，消费方
   从无源码包铸新实例化；格式版本抬升，全部`.ncu`随版本重编（既有惯例）。

## 四、非目标与边界

- 反射API：描述符兼为反射基础（`generics_design.md`§三），本版不交付反射面。
- 类型参数约束、模板特化、方法体内`new T[n]`、表达式位置显式泛型实参语法、
  预置容器实现改动（承`generics_design.md`§四非目标）。
- struct/func类型实参：多槽实参现拒绝；v0.13.2值类型引用（概要设计已于
  2026-11-07定稿并经三视角审核）解除struct实参。描述符表示须预留多槽实参的
  演进路径，本版不交付。

## 五、实施顺序与验收

- 顺序：catch拒绝独立可先行；描述符接线先于`typeName`与转型区分；`.ncu`
  模板表属序列化格式改动，可在本版内拆为独立步骤，不与小修复混排。
- e2e钉：`typeName`在共享方法体内返回规范拼写；`Box<int>`实例`as
  Box<string>`运行时失败（反向同）；`catch (Box<int> e)`编译期诊断；跨包
  （无源码`.npkg`）铸新实例化运行；T槽GC回归（generic_gc_tslot_survives族）；
  宿主ABI v12握手。
- 门禁含Debug断言构建冒烟；语言规格补`typeName`与catch边界说明；用户手册
  同步并以tools/manual_sync_check.py核销漂移。
