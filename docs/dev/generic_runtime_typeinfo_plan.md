# v0.13.1 运行时泛型类型参数信息——实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 接通泛型实例化的运行时类型信息：对象侧描述符载体、`typeName`表达式、
转型区分实例化、catch拒绝、T判等/拼接/`as T`恢复、`.ncu`模板表与宿主ABI v12。

**Architecture:** 字典模型下全部实例化共享模板行；实例化身份经进程级驻留的
`GenericInstDescriptor`（规范拼写键）携带——并行数组挂对象/数组记录（不动记录
布局与字段偏移），运行时类型信息操作经`this`读取；构造点写头支持静态拼写与
运行时合成配方两形态；`.ncu` v2.15序列化描述符表与模板标记。

**Tech Stack:** C++17 / CMake；flex+bison（typeName关键字与规则）；e2e=tests/e2e
（manifest.txt登记）；单元测试=ctest。

**Spec:** docs/dev/generic_runtime_typeinfo_design.md（概要设计，本计划据此展开；
D编号引用该文§三决策）。

## Global Constraints

- `NCU_FORMAT_MINOR` 14→15，loader拒绝≤14（T3一次到位，后续任务沿用15）。
- `NLANG_HOST_ABI_VERSION` 11→12（T13）。
- 不向下兼容；全部`.ncu`与native模块随版本重编。
- 注释统一英文；CRLF行尾；常量命名具体化；优先STL；`nlang::`命名空间。
- e2e新增测试须同步登记`tests/e2e/manifest.txt`（格式：`名称 期望退出码`）；
  既有期望被翻转的测试（err_generic_t_eq.n）在翻转任务内改写并更新manifest。
- 门禁：Debug（断言）＋ASan＋IDE树全量ctest，e2e串行；任一红=该批作废重扫。
- 版本收尾：VERSION→0.13.1、CHANGELOG.md与CHANGELOG.zh-CN.md成对、
  `tools/manual_sync_check.py --update`刷新基线。

## Review Focus（计划外风险位，各任务测试钉死）

1. **构造器体内typeName/判等**——OP_New须在ctor帧建立前完成写头（T4顺序）；
   T4/T6测试各含ctor体内读位。
2. **嵌套配方递归**——体内`Box<Box<T>>`构造的配方递归展开（T5测试）。
3. **跨单元描述符去重**——两单元各自实例化`Box<int>`，链接合并后转型互认
   （T3单测+T7 e2e，扩展proj_generic_cross_unit）。
4. **装箱/视图保描述符**——`Box<int>`装箱进Object、数组进Object再转回
   （T7 e2e；并行数组按heapIdx，装箱不改heapIdx）。
5. **并行数组增长原子性**——与`m_slotKinds`同点增长，gc-stress下描述符不丢
   （T4在GrowHeapShardBlockLocked单点追加；T15 ASan树e2e全量覆盖）。
6. **string实例化的null判等**——`T==null`句柄种类比0（null==null为true）、
   标量种类恒false（T9判等矩阵）。

---

### Task 1: catch泛型拒绝＋`as T`止血（D5/D6前置）

**Files:**
- Modify: `src/compiler/builder/StatementResolverSwitchTry.cpp:345-356`（catch门）
- Modify: `src/compiler/builder/ExprResolverCast.cpp`（as目标占位符拒绝）
- Test: `tests/e2e/err_catch_generic_type.n`（新）、`tests/e2e/err_as_type_param.n`
  （新，T10翻正）、`tests/e2e/manifest.txt`

**Interfaces:**
- Produces: 编译期诊断文案（T10沿用as位文案基调、T14手册引用catch文案）：
  - catch: `catch type cannot be a generic instantiation: exception dispatch matches the template row and cannot tell instantiations apart.`
  - as(T占位符): `cannot cast to a type parameter: runtime kind info is not wired yet.`（临时，T10替换）

- [ ] **Step 1: 写失败测试**（e2e两个，期望退出码=编译失败语义——查manifest既有
  err_*条目的登记形态，error测试的期望码跟随同族条目）

```nlang
// tests/e2e/err_catch_generic_type.n
class Box<T> { public Box() { } public T v; }
int main() {
    Box<int> b = Box();
    try { throw Exception("x"); }
    catch (Box<int> e) { return 1; }
    return 0;
}
```

```nlang
// tests/e2e/err_as_type_param.n
class C<T> {
    public C() { }
    public T f(Object o) { return o as T; }
}
int main() { return 0; }
```

- [ ] **Step 2: 登记 manifest 并跑，确认失败原因正确**

Run: `py313python tests/e2e/run_e2e_tests.py tests/e2e/err_catch_generic_type.n tests/e2e/err_as_type_param.n`
（按runner实际调用形态；预期两例当前行为=catch可编译运行（错）、as崩溃编译器）

- [ ] **Step 3: catch门**——`Access(SnCatchClause)`在`IsExceptionSubclass`检查后
  追加：解析结果`Field()`为`SnClassDecl`且`IsGenericInstantiation()`或
  `IsTypeParamPlaceholder()`→具名诊断。

- [ ] **Step 4: as止血**——ExprResolverCast.cpp处理SnAsExpr处，目标类型为占位符
  （`IsTypeParamPlaceholder()`）→具名诊断（不再落入后端崩溃路径）。

- [ ] **Step 5: 两例转绿＋既有e2e抽查（tests/e2e/全量快扫确认无回归）→Commit**
  `feat(diag): reject generic catch types and as-to-type-param`

### Task 2: TypeArgDesc完备化＋编译期描述符构造器（D2/D3地基）

**Files:**
- Modify: `src/vm/GenericInstDescriptor.h`（rank字段；`classRowRef`改名`rowRef`
  ——struct/class行引用按kind取义，无既有调用方，免费改名；头注释更新为限定名
  拼写契约）
- Create: `src/compiler/builder/GenericDescriptorBuild.cpp/.h`——
  `BuildTypeArgDescs(const SnClassDecl& inst, BuildEnvironment& env)`：按实例化
  节点的`GenericTypeArgs()`逐参构造`TypeArgDesc`（标量规范名/string/枚举限定名/
  类接口限定名＋行/结构体限定名＋行/数组元素递归＋秩/嵌套实例化递归/占位符
  特殊标记`kTypeParamArg=-2`）；`IsDescriptorRecipe()`判定实参表含占位符。
- Modify: `src/compiler/CMakeLists.txt`（新TU；若builder目标自动glob则免）
- Test: `tests/test_vm/test_generic_descriptor.cpp`（扩展既有文件）

**Interfaces:**
- Produces: `TypeArgDesc{spelling, resolvedKind, rowRef, rank, nested}`；
  `std::vector<TypeArgDesc> BuildTypeArgDescs(...)`；占位符实参的
  `rowRef=kTypeParamArgBase-序号`编码（消费方T5解码）；限定链接名取自
  `SnClassDecl`的注册键（与`NamedTypeExprFromKey`消费的键同源——
  CompiledModuleStubSupport.hpp:55注释）。

- [ ] **Step 1: 失败单测**——BuildTypeArgDescs对具体实例化产出正确拼写/种类/
  行引用（int、string、限定类、struct、int[]秩1、int[,]秩2、嵌套）；对含T
  实参产出占位符编码＋IsDescriptorRecipe真。
- [ ] **Step 2: 跑测确认失败**（符号不存在）
- [ ] **Step 3: 实现**（规范名表用`kScalarPrims`；限定名走注册表键）
- [ ] **Step 4: 转绿；InternDescriptor拼写往返单测（结构↔拼写再生恒等）→Commit**
  `feat(gen): canonical descriptor builder for instantiations`

### Task 3: 模块描述符表＋`.ncu` v2.15＋loader＋链接合并（D3/D7）

**Files:**
- Modify: `include/nlang/vm/CompiledModule.h`（`NCU_FORMAT_MINOR`=15＋版本注释；
  `CompiledModule`追加`std::vector<DescriptorEntry> descriptorTable`，
  `DescriptorEntry{std::string spelling; std::vector<uint8_t> recipe;}`——
  recipe为空=静态拼写；非空=配方字节码（T5定义编码，本任务先落串表与空配方）
  ；表在`stringConstants`之后）
- Modify: `src/vm/ModuleSaver.cpp:110-117`后插新段（u32 count；每条目
  u16 spellingLen+bytes＋u16 recipeLen+bytes）
- Modify: `src/vm/ModuleLoader.cpp`（镜像读取；版本门14→15拒绝下限）
- Modify: `src/vm/NcuLinker.cpp`＋`NcuLinkerRemap.h`（合并时描述符表按拼写
  去重、各单元函数字节码中新操作数的descIdx重映射——沿用既有字符串常量
  重映射的同构走查；本任务先落表合并，操作数重映射随T4引入操作数时在同
  一走查switch中挂臂）
- Test: `tests/test_vm/test_module_save_load`（扩展：含描述符表的roundtrip＋
  旧版本拒绝）

- [ ] **Step 1: 失败单测**（save→load描述符表roundtrip；v2.14镜像拒绝）
- [ ] **Step 2: 确认失败→实现saver/loader/版本→转绿**
- [ ] **Step 3: 手编最小`.ncu`十六进制核对可选跳过（roundtrip已钉）→Commit**
  `feat(ncu): v2.15 module descriptor table`

### Task 4: 执行器并行数组＋OP_New写头·静态拼写（D1载体落地）

**Files:**
- Modify: `src/vm/VmExecutor.h/.cpp`（成员`ChunkedArray<GenericInstDescriptor*>
  m_instDescByHeap`；`GrowHeapShardBlockLocked`（VmExecutorAlloc.cpp:31-39）同点
  `Emplace(nullptr)`；`ResetPerRunState`清空）
- Modify: `src/vm/VmExecutorOpsObjects.cpp:123-129`（`OpNew`读追加的u16
  descIdx；`AllocClassOnHeap`（VmExecutorAlloc.cpp:82）加desc参数写并行数组；
  加载期描述符表预驻留：`InitStringStore`同段位把`descriptorTable`逐条
  `InternDescriptor`化缓存为指针表）
- Modify: `src/vm/BytecodeOps.h`（OP_New操作数注释更新）；`Disassembler.cpp`
  同步
- Modify: 发射侧三位点带静态descIdx（具体实例化）：`src/vm/backend/EmitCtorCall.cpp:103`、
  `EmitExprNew.cpp:97`、`EmitExprInitList.cpp:165`——实例化节点经Task2构造器
  得实参表，无占位符→注册`descriptorTable`静态条目取idx；有占位符→本任务暂
  发0xFFFF（T5补配方）；非模板行→0xFFFF
- Test: `tests/test_vm/test_generic_runtime_info.cpp`（新）：编译运行最小
  `Box<int>`程序，探针访问`m_instDescByHeap`断言拼写（进程内宿主模式，
  参照test_generic_descriptor的宿主初始化纪律——`Runtime::StaticInit()`先调）

**Interfaces:**
- Produces: 执行器只读视图`const GenericInstDescriptor* InstDescriptorOf(int32_t
  heapIdx) const`（T5-T11消费）；发射侧辅助`uint16_t VmBackend::
  DescriptorIndexFor(SnClassDecl& inst)`（T5/T7/T8消费，含驻留注册）。

- [ ] **Step 1: 失败单测**（构造`Box<int>`后InstDescriptorOf→"Box<int>"拼写；
  非泛型对象→nullptr）
- [ ] **Step 2: 确认失败→实现→转绿**
- [ ] **Step 3: e2e快扫（构造族全部既有测试不回归——OP_New操作数变化动全部
  构造，必须全绿）→Commit** `feat(vm): instantiation descriptors on objects`

### Task 5: 运行时合成配方＋静态体内构造拒绝（D3）

**Files:**
- Modify: `include/nlang/vm/CompiledModule.h`（recipe编码定案：字节码=
  `u8 tag`序列，tag 0=TEMPLATE_HEAD(u16 nameLen+bytes)｜1=ARG_CONST(u16
  spellingLen+bytes)｜2=ARG_THIS(u8 paramIdx)｜3=ARG_ARRAY(u8 rank+子编码)｜
  4=ARG_NESTED(子配方)——T3的`recipe`字节vector载体直接用）
- Modify: `src/vm/backend/EmitCtorCall.cpp`等三位点（Task4的0xFFFF占位改配方
  注册：实参表含占位符→按编码生成recipe注册表取idx）；静态上下文（发射时
  当前函数`isStatic`且属模板行）→具名拒绝`constructing a generic type with
  type-parameter arguments requires an instance context.`
- Modify: `src/vm/VmExecutorOpsObjects.cpp`（OpNew带配方臂：读帧`this`（局部
  槽0）heapIdx→InstDescriptorOf→逐tag组装TypeArgDesc表→InternDescriptor→写
  并行数组；null this防御断言）
- Test: `tests/test_vm/test_generic_runtime_info.cpp`扩展

- [ ] **Step 1: 失败单测**——`Hold<T>`体内`Box<T> b = Box(x)`运行后
  `InstDescriptorOf(b)=="Box<int>"`（Hold<int>驱动）；嵌套`Box<Box<T>>`同；
  静态体内构造报编译错
- [ ] **Step 2: 确认失败→实现→转绿→Commit** `feat(vm): runtime descriptor recipes`

### Task 6: `typeName(类型)`表达式（D4）

**Files:**
- Modify: `src/compiler/grammar/nlang.l`（关键字`typeName`→新token KT_TypeName）
- Modify: `src/compiler/grammar/nlang.y`（Primary层新规则
  `TypeNameExpr: KT_TypeName '(' Type ')'`；Type复用声明位Type非终结——
  落点参照`nlang.y:638`HeadType族的Type定义；新建`SnTypeNameExpr`节点）
- Modify: AST节点X-macro清单（`SyntaxTree.h`节点族——新节点
  `SnTypeNameExpr(SnFieldExpr* type, loc)`；Visitor臂生成）
- Modify: `src/compiler/builder/ExprResolverValues.cpp`（Access(SnTypeNameExpr)：
  先解析类型子树；具体类型→折叠为SnLiteralExpr(string规范拼写——复用Task2
  构造器拼段)；裸T占位符→记录参数序号待发射；T嵌实参→运行时合成标记；
  静态上下文→具名拒绝`typeName of a type parameter requires an instance
  context; use an instance member or the compensation pattern.`）
- Modify: `src/vm/BytecodeOps.h`＋执行器＋Disassembler（新
  `OP_TypeNameArg u16 dst, u8 argIdx`——读this描述符argIdx拼写建字符串；
  `OP_TypeNameCompose u16 dst, u16 recipeIdx`——配方合成拼写建字符串，配方
  复用Task5编码的串段变体：tag集增加CONST_SPELLING段即可）
- Modify: `src/vm/backend/EmitExprValues.cpp`（或SnTypeNameExpr发射归属文件）
- Test: e2e `generic_typename_fold.n`（具体类型折叠：int/Box<int>/int[]/限定名/
  嵌套List<List<int>>）＋`generic_typename_runtime.n`（T各实例化）＋
  `err_typename_static_context.n`＋manifest登记

- [ ] **Step 1: e2e三例失败测试写好＋登记**
- [ ] **Step 2: 确认失败（语法错）→Step 3: 扫描器/文法/AST/解析/发射/执行器实现**
- [ ] **Step 4: 转绿＋既有e2e快扫→Commit** `feat(lang): typeName expression`

### Task 7: 转型区分实例化（D5主体）

**Files:**
- Modify: `src/vm/BytecodeOps.h`（OP_CheckCast操作数+u16 descIdx）；Disassembler
  同步
- Modify: `src/compiler/CastInfo.cpp:194-249`（类臂：目标为实例化节点→
  TCK_Downcast/Same附描述符（经Task2构造器）；同模板不同实参→维持TCK_None
  编译期拒绝不变）
- Modify: `src/vm/backend/EmitExprCast.cpp:263-280`（TCK_Downcast发射带
  descIdx=DescriptorIndexFor；所有OP_CheckCast发射位点清点补操作数——
  `EmitArrayBoundary.cpp:47`下落臂同）
- Modify: `src/vm/VmExecutorOpsObjects.cpp`（OpCheckCast读descIdx：行匹配
  通过且descIdx≠0xFFFF→InstDescriptorOf比对，失配抛InvalidCastException
  消息含两侧Spelling()；目标为Array面行且操作数为数组→转Task8数组比较）
- Test: e2e `generic_cast_distinguish.n`（Box<int>经Object转Box<string>抛
  InvalidCastException被catch；正向通过；装箱中转；反向）＋
  proj_generic_cross_unit追加跨单元互认例

- [ ] **Step 1: e2e失败测试（当前Object中介转错误通过=测试红）**
- [ ] **Step 2: 实现→转绿→Commit** `feat(vm): instantiation-aware casts`

### Task 8: 数组元素位描述符＋预置洞（D5数组位）

**Files:**
- Modify: `src/vm/BytecodeOps.h`（OP_AllocArrayN/OP_AssembleArrayN＋u16
  descIdx——元素类型为实例化时非0xFFFF）
- Modify: `src/vm/backend`数组创建发射位（清点：声明位/字面量位/`new`位——
  `EmitExprNew.cpp`族；元素为实例化→DescriptorIndexFor；元素为裸T→维持
  既有拒绝）
- Modify: `src/vm/VmExecutorOpsArrayCheck.cpp`（OP_CheckArrayType与cast的
  数组比较键扩：elemKind、elemTypeIdx、rank相等且元素类型为模板行→并行
  数组元素描述符位与目标描述符实参0比对；同时修复`Object o=int数组;
  o as Array<string>`静默通过的既有洞）
- Test: e2e `array_cast_generic_elem.n`（Box<int>[]与Box<string>[]经Object
  互转一过一抛；Array<int>与Array<string>同）＋manifest

- [ ] **Step 1: 失败e2e（两洞当前都漏）→Step 2: 实现→转绿→Commit**
  `feat(vm): array element descriptors close instantiation cast holes`

### Task 9: T判等/拼接/toString解禁（D6主体一）

**Files:**
- Modify: `src/compiler/builder/ExprResolverTypeGates.cpp:124-152`
  （RejectTypeParamOperand收窄：`==`/`!=`当两操作数皆T、或一侧T一侧null
  →放行新路径；其余维持拒绝）；`ExprResolverBinary.cpp:469`（Eq/Ne占位符
  组合→求值为bool并标记发射OP_Eq_T族；`+`当一侧string一侧T→拼接标记）
- Modify: `src/vm/BytecodeOps.h`＋执行器＋Disassembler：
  `OP_Eq_T u16 dst, u8 argIdx, u16 lhs, u16 rhs`（null侧=0xFFFF特殊槽
  编码或专用OP——落位时择简）；`OP_TypeArg_to_str`（种类分派toString，
  消费0.13.0的分派例程——StructValuesEqual/OP_Eq_str/装箱渲染同族位）
- Test: e2e `generic_t_eq_matrix.n`（int/string/struct/class/enum/array实例化
  ×等/不等/null三形态）＋`generic_t_concat_print.n`＋改写
  `err_generic_t_eq.n`→正例并入matrix（manifest同步）；err_generic_t_arith/
  order维持（快扫确认仍拒）

- [ ] **Step 1: 判等矩阵e2e写好（当前全被拒=红）→Step 2: 实现→转绿**
- [ ] **Step 3: 拼接/打印e2e→实现→转绿→Commit** `feat(lang): kind-dispatched equality and toString on T`

### Task 10: `as T`分派（D6主体二，翻正T1）

**Files:**
- Modify: `src/compiler/builder/ExprResolverCast.cpp`（T1临时拒绝改放行：
  目标占位符→标记发射）
- Modify: `src/vm/backend/EmitExprCast.cpp`（新OP `OP_As_TypeParam u8 argIdx`
  ——pResult进出；null直通）
- Modify: 执行器（读this描述符argIdx种类分派：标量→拆箱检查（复用
  RTK_Boxed拆箱路径）；string→种类检查；struct→身份＋深校验；class/interface
  →行行走＋描述符递归（实参为实例化时）；enum→内联；array→Task8比较键；
  失败抛InvalidCastException消息含目标拼写）
- Test: e2e `generic_as_type_param.n`（各实例化双向＋失败路径catch）＋
  删除`err_as_type_param.n`（manifest同步）

- [ ] **Step 1: e2e失败（当前拒绝）→Step 2: 实现→转绿→Commit**
  `feat(lang): kind-dispatched as-to-type-parameter`

### Task 11: 序列化往返携带描述符（D3序列化位）

**Files:**
- Modify: `src/vm/IntrinsicsFileStream.cpp`/`IntrinsicsByteStream.cpp`的
  WriteObject/ReadObject类对象臂（写：模板行对象先写描述符拼写串；读：
  AllocClassOnHeap后InternDescriptor写并行数组；向后不兼容——旧流不含拼写
  则按无描述符读，仅同版本流互转，符合不向下兼容约束但读写对称须钉测试）
- Test: e2e `generic_serialize_roundtrip.n`（Box<int>含T字段写读后typeName
  与转型正确）

- [ ] **Step 1: 失败e2e→Step 2: 实现→转绿→Commit**
  `feat(ser): carry instantiation descriptors through object graphs`

### Task 12: `.ncu`模板表与无源码铸造（D7主体）

**Files:**
- Modify: `include/nlang/vm/CompiledModuleRecords.h`（CompiledClass追加
  `bool isTemplate=false; std::vector<std::string> typeParamNames;`）
- Modify: `src/vm/ModuleSaverRecordsClasses.cpp`/`ModuleLoaderRecordsClasses.cpp`
  （类记录尾两段：u8标记＋u16 count＋逐名；版本15内兼容=直接追加）
- Modify: `src/vm/TypeDesc.cpp`（`RTK_TypeParam`叶入wire：kind字节＋u8参数
  序号；先核实`backend/Types.cpp:40`既有映射是否已让paramTypeDescs携带
  RTK_TypeParam、仅AppendTypeDescBytes降级——按实际补写读臂）
- Modify: `src/compiler/builder/CompiledModuleStubSupport.hpp`（
  `SynthTypeExprFromDesc`加RTK_TypeParam臂：合成`SnIdentifierExpr(参数名)`——
  由类记录typeParamNames按序号取名）；桩重建器（ClassStubBuilder落位文件）
  对isTemplate行：`SetTypeParamNames`＋`SetGenericDeclaration`，成员签名重建
  走占位符绑定（BindTypeParamPlaceholderIfAny父链查找自然命中）
- Modify: `src/vm/backend/Register.cpp`/`Slots.cpp`（模板行成员记录经导入
  槽解析已有；核实mint对桩模板的AdoptMemberClone/RecordOwnerOfMember链路）
- Test: e2e新proj `proj_generic_sourceless`（生产单元仅以`.npkg`（剥离`.n`源）
  分发，消费单元import后`Box<int>`构造/方法/`typeName`运行）＋单测桩重建

- [ ] **Step 1: proj失败e2e搭好（当前桩上实例化不可铸=红）**
- [ ] **Step 2: wire＋桩实现→转绿**
- [ ] **Step 3: proj_generic_cross_unit快扫无回归→Commit**
  `feat(ncu): template table and sourceless instantiation`

### Task 13: 宿主ABI v12（D8）

**Files:**
- Modify: `include/nlang/vm/NativeHost.h`（版本注释＋12；表尾三回调：
  `uint32_t (*typeArgCount)(NativeHost*)`、`uint8_t (*typeArgKind)(NativeHost*,
  uint32_t i)`、`const char* (*typeArgName)(NativeHost*, uint32_t i)`——
  返回指针至下一回调前有效）
- Modify: `src/vm/VmExecutorHostBridge.cpp`（回调实现：当前native调用的
  接收者heapIdx→InstDescriptorOf→逐实参回答；无描述符→count 0）
- Modify: `src/vm/NativeLibraryLoader.cpp`（版本门11→12）
- Modify: stdlib native模块的`nlang_abi_version`导出与重建（`nlang_io`等
  ——全工具链重编覆盖）
- Test: 单测（宿主侧回调契约：模板行接收者三回调返回正确、非模板count 0）
  ＋既有native e2e全绿（版本握手）

- [ ] **Step 1: 失败单测→Step 2: 实现→转绿→Commit** `feat(abi): v12 type-argument callbacks`

### Task 14: 文档与版本批（D9）

**Files:**
- Modify: `docs/user_manual/{zh,en}/specification/04-types/generics.md`（§4.12.2
  判等段改写——解除`==`拒绝表述、补typeName小节、体内构造与配方语义、转型
  区分、catch拒绝；§4.12.8跨单元补模板表一句）、`05-expressions/operators.md`
  （T判等/拼接）、`08-exceptions`（catch泛型拒绝）、`formats/`（ncu v2.15段）
- Modify: 用户手册补偿模式（§4.12新增小节：非泛型辅助类
  `static Dict<string,...>`按`typeName(T)`分键，中英成对）
- Modify: `CHANGELOG.md`＋`CHANGELOG.zh-CN.md`（0.13.1条目成对）、`VERSION`
  →0.13.1、`README.md`（如语言面表格提及）、`include/nlang/vm/NativeHost.h`
  已在T13、`docs/roadmap.md`（0.13.1条目落完成态）
- Verify: `py313python tools/manual_sync_check.py`零漂移后`--update`

- [ ] **Step 1: 中文树编辑→style单测＋缩略语自查（页级全称共现grep）**
- [ ] **Step 2: 英文树镜像→Step 3: CHANGELOG/VERSION/roadmap→Commit**
  `docs: v0.13.1 runtime generic type information`

### Task 15: 门禁收尾与循环完善

- [ ] **Step 1: 本任务前全任务`审核-验证-完善`循环——代码质量＋架构双维
  （superpowers requesting-code-review），中高级问题清零**
- [ ] **Step 2: Debug树构建＋ctest全量；ASan树构建＋e2e全量（工具目录补native
  DLL与asan运行库、stdlib重编——stride别名门禁配方）；IDE树全量ctest（139+
  Qt族）**
- [ ] **Step 3: e2e基线数对拍（当前1793＋新增）→任一红全批重扫**
- [ ] **Step 4: manual_sync_check零漂移→roadmap当前状态更新→终Commit＋push
  （显式代理配置）＋tag v0.13.1＋本地部署d:/dev/nlang（装前关nide、装后
  哈希对拍）**

---

## 自查记录（writing-plans Self-Review）

- **Spec覆盖**：D1→T4/T5；D2→T2；D3→T4/T5/T11；D4→T6；D5→T7/T8/T1(catch)；
  D6→T9/T10；D7→T3/T12；D8→T13；D9→T14/T15——全覆盖。
- **占位符扫描**：无TBD；两个"落位时择简"为受限实现自由度（语义已钉）。
- **类型一致性**：InstDescriptorOf/DescriptorIndexFor/BuildTypeArgDescs/
  DescriptorEntry在各任务间签名一致；recipe编码T3落载体、T5定码、T6复用。
- **Review Focus**：6条均有归属任务测试。
