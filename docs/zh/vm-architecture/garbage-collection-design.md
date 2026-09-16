# 垃圾回收设计


### 设计决策

四个关键决策及其依据记录在 VmExecutor.h 中：

**1. 由安全点触发，而非由分配点触发**

在分配点（OP_New/OP_AllocStruct）处，tempSlot 与 tempSlot2 可能正持
有活跃堆引用（例如 `a.b.c = new Foo()`，此时 tempSlot=a、
tempSlot2=a.b）。追踪这些临时值需要 CompiledFunction 里配备逐指令的
偏移表，复杂度不划算。

安全点（函数入口 + 循环回边）保证 tempSlot/tempSlot2 未持有活跃引
用：函数入口处栈帧刚刚建立；循环回边处条件即将重新求值，临时值会被
消费掉。

**2. 经 LocalDescriptor 精确扫描，而非保守字节扫描**

保守扫描（检查每个 4 字节对齐的值是否为有效堆索引）把 GC 与物理栈
帧布局（VALUE_SIZE、槽位对齐、tempSlot 偏移）耦合在一起。布局一旦
变化，GC 会悄无声息地失效。

精确扫描用 `LocalDescriptor.typeKind` 识别引用槽位，用
`LocalDescriptor.offset` 定位。它基于语义信息而非物理布局，因此无论
栈帧布局如何变化，GC 都保持正确。

**3. 用 m_slotStructIdx 并行数组识别 struct 类型**

struct 对象没有类型头（不同于 class 对象的 slot[0] = classIdx）。
MarkStruct 需要 structIdx 才能查询字段布局、追踪引用。可选方案：
- 给 struct 对象加类型头：所有针对 struct 的 LoadField/StoreField
  偏移都要 +1，牵动大量指令处理器
- 并行数组：只有 AllocStructOnHeap/DeepCopyStruct 需要设置它，既有
  指令逻辑零改动

并行数组是更小、更安全的改动。

**4. 迭代式标记 + 工作列表，而非递归**

递归的 MarkObject/MarkStruct 在深对象链（例如 500+ 节点的链表）上会
溢出 C++ 调用栈。迭代方案用一个 `vector<int32_t>` 工作列表：MarkPhase
识别根引用并压入工作列表，然后迭代处理条目直至工作列表清空；每个条目的
子引用若未标记则压入工作列表。内存用量以此为界（O(可达对象数)），没有
栈溢出风险。

### GC 算法

```text
CheckGCSafepoint():
  if m_gcPending && heap.size() > threshold:
    m_gcPending = false
    CollectGarbage()

CollectGarbage():
  MarkPhase()
  SweepPhase()

MarkPhase():
  clear all mark bits
  for each CallFrame:
    for each LocalDescriptor with typeKind in {RTK_Class, RTK_Struct, RTK_Array, RTK_Func}:
      read heap index from frame.locals + ld.offset
      if valid and not marked: set mark bit, push to worklist
    if pResult has reference return type:
      read heap index from pResult
      if valid and not marked: set mark bit, push to worklist
  while worklist not empty:
    pop entry from worklist
    if class: for each field, push unmarked reference children
    if struct: for each field, push unmarked reference children
    if array: push unmarked element records whose elemKind is a reference kind

SweepPhase():
  clear free list
  for each heap slot:
    if not free and not marked:
      if class: FreeOwnedStructs (free value-owned struct fields)
      if struct: FreeNestedStructs (free nested struct fields)
      clear slot, mark as free, add to free list
```

### 数组字段与元素追踪（数组重设计 B）

- **数组类型字段是声明出来的，不是推断出来的**：array 型 struct/
  class 字段在 `.nmod` 里的 `fieldTypeKinds` 条目直接存 `RTK_Array`
  （语义下限 v1.11）。MarkPhase 对 class/struct 字段引用采用「声明
  kind + 运行期槽位 kind」双重条件路由——`fieldTypeKinds[i] ==
  RTK_Array` 且该槽位确实持有数组记录——与既有的
  RTK_Class/RTK_Struct/RTK_Func 分支并列。
- **旧的运行期 kind 兜底已移除**：MarkPhase 不再仅因某字段槽位的运
  行期 kind 看起来像数组就去追踪。这是安全的，因为锯齿声明
  （`T[][]`）——唯一可能把数组记录塞进非数组类型字段槽位的书写形式
  ——已在 resolve 阶段被编译器拒绝。
- **防御性 RTK_Array 元素分支**：数组分支会追踪声明 `elemKind` 为
  `RTK_Array` 的元素（与字段分支相同的双重条件）。如今从可编译源码
  出发不可达（锯齿声明被拒绝）——它是未来或外部产出的 `.nmod` 路径
  的正确性基座。数组类型的*容器*元素（`List<int[]>`、`Dict` 的键/
  值）改由容器分支追踪：List/Dict 分支标记并压入引用 kind 的条目，
  数组元素由此进入工作列表的 `RTK_Array` 分支，按 elemKind 追踪数组自
  身的元素（`List<Point[]>` 就是这样让 `Point` 记录存活的）。

### 空闲表集成

各分配函数（AllocClassOnHeap、AllocStructOnHeap、DeepCopyStruct）先
查空闲表：有空闲槽位就复用（resize 到正确大小，设置
m_slotKinds/m_slotStructIdx）；否则 emplace_back 一个新槽位。

### GC 触发流程

```text
OP_New / OP_AllocStruct → set m_gcPending = true
                            ↓
Function entry (ExecuteFunction) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && heap > threshold → CollectGarbage()
```
