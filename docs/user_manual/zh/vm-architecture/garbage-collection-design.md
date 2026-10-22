# 垃圾回收设计

NLang的堆由垃圾回收（GC，garbage collection）自动管理：开发者创建对象后无需手工释放。本页记录四个核心设计决策——安全点触发、按槽位句柄位图的精确扫描（带保守回退）、并行数组、迭代式标记——以及完整的标记-清扫算法、停泊上下文与在途阻塞原生的根集、数组字段的追踪规则。读懂这些决策就能知道VM会在哪些时刻回收、哪些槽位会被视为存活。

### 设计决策

四个关键决策及其依据记录在VmExecutor.h中：

**1. 由安全点触发，而非由分配点触发**

在分配点（OP_New/OP_AllocStruct）处，tempSlot与tempSlot2可能正持有活跃堆引用（例如`a.b.c = Foo()`，此时tempSlot=a、tempSlot2=a.b）。追踪这些临时值需要CompiledFunction里配备逐指令的偏移表，复杂度不划算。

安全点（函数入口 + 循环回边）把回收放在边界清晰的位置：函数入口处栈帧刚刚建立；循环回边处条件即将重新求值。历史上这个选择还依赖「回边处临时槽不持有活跃引用」的论证；引入句柄位图（决策2）后，暂存槽在任何安全点都被覆盖，该论证不再承重——安全点触发保留的价值是标记工作的时机确定、成本可控。

**2. 按槽位句柄位图给出精确根类别**

对整帧做保守字节扫描（把每个对齐值当候选堆索引检查）曾是被否决的方案：它把GC与物理栈帧布局（槽位宽度、对齐、tempSlot偏移）耦合在一起，布局一旦变化，GC会悄无声息地失效。槽位因此携带语义信息。参数走声明路线（`LocalDescriptor.typeKind`识别引用槽位，`LocalDescriptor.offset`定位）。帧带`[paramCount, localsSize)`内的其余槽位——返回槽、临时池、调用实参暂存区、求值暂存区与命名的局部变量——由随函数记录一起序列化的**句柄位图**（NCU格式2.8）分类：每槽2位，共四种类别。

- `None`：标量；槽位永不持有引用，不测试。
- `StrHandle`：字符串仓句柄；只测字符串域。
- `HeapIndex`：struct/class/数组/函数的堆索引；只测堆域。
- `Dual`：未知；双域值测试——即保守测试本身，作为回退类别保留。

后端由栈帧布局推导类别：求值区之下的固定暂存区（临时池与调用暂存）为`Dual`，因为任何表达式都可能流经；命名的局部变量与返回槽取各自声明的种类；求值区槽位初值为`None`，由使用它的代码位点所声明的类别（`EvalAreaClaim`）加宽。只有能枚举全部过路值的位点才可声明窄类别——例如委托调用的类别是`HeapIndex`，因为该槽只由一条`OP_LoadField`写入。若同一槽位还可能接收表达式中间值，声明保持`Dual`；被多个位点声明的槽位取各类别的并集（按位或），只会向`Dual`加宽。

意义所在：声明表看不见暂存槽。`f(g(), h())`中g的返回值在h入帧（安全点）时只活在调用帧的求值暂存区里。位图以每次收集更少的测试工作补上这个洞，且不依赖物理布局：只有`Dual`槽位才付双域值测试的代价。帧建立时零初始化，未写的槽值为0，两个域都不命中；GC不移动对象，因此`Dual`槽位的跨域命中只可能多标（对象暂留），绝不漏标。手工构造的内存函数记录若位图长度与帧布局不符（装载器在产物中拒绝这种形态），整带回退为双域保守扫描。

停泊上下文的帧可能冻结在指令中途，暂存值同样必须存活——位图对停泊帧与运行帧一视同仁（见「停泊上下文与调度器根集」），不需要为停泊形态另建一套根集机制。

**3. 用m_slotStructIdx并行数组识别struct类型**

struct对象没有类型头（不同于class对象的slot[0] = classIdx）。MarkStruct需要structIdx才能查询字段布局、追踪引用。可选方案：
- 给struct对象加类型头：所有针对struct的LoadField/StoreField偏移都要+1，牵动大量指令处理器
- 并行数组：只有AllocStructOnHeap/DeepCopyStruct需要设置它，既有指令逻辑零改动

并行数组是更小、更安全的改动。

**4. 迭代式标记 + 工作列表，而非递归**

递归的MarkObject/MarkStruct在深对象链（例如500+节点的链表）上会溢出C++调用栈。迭代方案用一个`vector<int32_t>`工作列表：MarkPhase识别根引用并压入工作列表，然后迭代处理条目直至工作列表清空；每个条目的子引用若未标记则压入工作列表。内存用量以此为界（O(可达对象数)），没有栈溢出风险。

### GC算法

```text
CheckGCSafepoint():
  if m_gcPending && (heap.size() > threshold || strings.size() > strThreshold):
    m_gcPending = false
    CollectGarbage()

CollectGarbage():
  MarkPhase()
  SweepPhase()
  SweepStrings()

MarkPhase():
  clear all mark bits (heap and string stores)
  for each CallFrame (running stack):
    //parameters: declared descriptors
    for each LocalDescriptor with isParam and a reference typeKind:
      RTK_String: MarkString(value)      //handle, not heap index
      otherwise:  if valid and not marked: set mark bit, push to worklist
    //return slot: declared return kind
    if returnTypeKind is a reference kind:
      RTK_String: MarkString(value)      //conservative over-mark, as defense
      otherwise:  mark and push if valid
    //band: two-bit root class per slot (NCU 2.8 handle bitmap)
    for each band slot i, slot number paramCount + i:
      class = (func.evalAreaHandleMask[i >> 2] >> ((i & 3) * 2)) & 3
      None:      skip
      StrHandle: MarkString(value)
      HeapIndex: mark and push if valid
      Dual:      value-test both domains
    //fallback for a bitmap whose length disagrees with the frame layout:
    //value-test both domains over the whole band
  for each scheduler context:      //parked/queued contexts (steal model)
    for each parked CallFrame: run the same three arms (params, result, band)
    if threadObjHeapIdx or failExcHeapIdx valid: mark, push to worklist
  for each in-flight blocking native call:
    staged argument cells and return slot: value-test both domains
    handles minted by re-entrant callbacks: value-test both domains
  for each host-registered root:
    string roots to MarkString, heap roots to worklist
  while worklist not empty:
    pop entry from worklist
    if class: for each field, push unmarked reference children;
      string-typed fields (RTK_String) mark their string handle instead;
      MarkChannelElements (buffered channel values are children)
    if struct: for each field, push unmarked reference children;
      string-typed fields mark their string handle instead
    if array: push unmarked element records whose elemKind is a reference kind;
      string elements (elemKind == RTK_String) mark their string handle
    if boxed: a string-tagged payload (slot[1]) marks its string handle;
      other boxed tags carry raw bits (no children)

SweepPhase():
  clear free list
  for each heap slot:
    if not free and not marked:
      if class: FreeOwnedStructs (free value-owned struct fields)
      if struct: FreeNestedStructs (free nested struct fields)
      clear slot, mark as free, add to free list

SweepStrings():
  clear string free list
  for each string object (slot 0 is the null sentinel):
    if dead: continue
    if immortal (constant-materialized) or marked: count survivor, continue
    if interned: erase its entry from the short-string table
    reset slot to the dead-form sentinel, add to string free list
  strThreshold = max(strThreshold, 2 * survivorCount)
```

**字符串对象仓的独立清扫。**字符串对象存放在独立的仓中（见[堆架构](heap-architecture.md)页），拥有自己的标记位向量、空闲表与回收阈值，但复用同一次`CollectGarbage`：一个标记阶段同时填充两份位向量，堆清扫之后字符串仓紧接着清扫。随模块执行物化的常量对象带immortal位，永不回收；被回收的驻留短串在同一步里净化驻留表条目，保证「相同内容至多有一个存活驻留对象」的不变量在收集中不被打破。

**字符串分支的阈值退避。**字符串仓的底层数组从不缩容（死槽原地复用），因此尺寸触发是电平触发的——首次越限后每个安全点都会触发回收，而每次回收都要标记整条存活的拼接链，追加总量退化为O(n²)（基准：5×10^4节点链6.6s、10^5节点链26.7s）。每次清扫后把阈值退避到存活数的2倍（`strThreshold = max(strThreshold, 2 * survivorCount)`），触发点随存活集合几何级推进：摊还后追加O(1)，内存上界为存活集合的2倍。

### 停泊上下文与调度器根集

帧栈机下，正在执行的上下文的帧就是调用栈本身；**停泊**（睡眠、等待join、`Future.get`取值、竞争互斥体、空通道接收、满通道发送、被抢占）的上下文的整条帧栈保存在调度器里，不在调用栈上。收集时二者都是根：

- 每个停泊上下文的每个帧跑与运行帧相同的三个扫描臂（参数描述符、返回值槽、帧带位图）——停泊可能发生在指令中途，帧内暂存值与处理器状态都必须存活；
- 上下文的`Thread`对象（`join`按其句柄识别目标）与失败上下文保留的异常对象（`join`要按原对象重抛）单独作为根；
- 通道缓冲里的值经工作列表的class分支追踪（`Channel<T>`是带句柄字段的内建class，缓冲元素随对象标记）。

`thread.spawn`在铸好根帧与`Thread`对象后即经过一次安全点检查，派生上下文的帧从诞生起就被根集覆盖。上下文与并发语义的总览见[并发](../language-spec/concurrency.md)与[帧机与调度器](frame-machine-and-scheduler.md)。

### GIL下的回收与阻塞原生扎根

堆的每一次改动——分配、标记、清扫——都持有全局解释器锁（GIL，global interpreter lock；见[并发](../language-spec/concurrency.md)）。回收在持有GIL的载体上运行，其余载体此时无法触碰堆：不需要另设停止世界的握手。

- 非阻塞原生调用全程持有GIL，回收不可能与其交错，实参无需额外扎根。
- **阻塞**原生调用使用暂存格（staged cells）：此类调用按ABI（应用二进制接口，application binary interface）第5版的注册通道声明。VM把实参拷入自有格子，释放GIL，调用原生，重新取GIL（先查停止标志），复原帧，再把结果拷回。无锁窗口内其他载体上的回收可以运行，因此暂存的实参格与返回槽在整个调用期间都是根；返回格按地址作为根——原生自己写入其中。
- 阻塞原生内对VM的回调（构造字符串、构造列表、抛出异常）每次回调重新获取GIL。它们铸造的句柄压入执行器的本地根向量，调用返回时截断到水印；它存放在执行器而非载体的C++栈上，因为运行在另一载体上的回收器看不见那条栈。
- 根向量的改动都在GIL下进行（释放前登记，重新获取后截断），回收读到的列表因此始终一致。

### 数组字段与元素追踪

- **数组类型字段是声明出来的，不是推断出来的**：array型struct/
  class字段在`.ncu`里的`fieldTypeKinds`条目直接存`RTK_Array`。MarkPhase对class/struct字段引用采用「声明kind + 运行期槽位kind」双重条件路由——`fieldTypeKinds[i] ==
  RTK_Array`且该槽位确实持有数组记录——与既有的RTK_Class/RTK_Struct/RTK_Func分支并列。
- **字段追踪只看声明kind**：MarkPhase不会仅因某字段槽位的运行期kind看起来像数组就去追踪。这是安全的，因为锯齿声明（`T[][]`）——唯一可能把数组记录塞进非数组类型字段槽位的书写形式——已在resolve阶段被编译器拒绝。
- **防御性RTK_Array元素分支**：数组分支会追踪声明`elemKind`为`RTK_Array`的元素（与字段分支相同的双重条件）。该分支从可编译源码出发不可达（锯齿声明被拒绝）——它是外部产出的`.ncu`路径的正确性基座。数组类型的*容器*元素（`List<int[]>`、`Dict`的键/值）改由容器分支追踪：List/Dict分支标记并压入引用kind的条目，数组元素由此进入工作列表的`RTK_Array`分支，按elemKind追踪数组自身的元素（`List<Point[]>`就是这样让`Point`记录存活的）。

### 空闲表集成

各分配函数（AllocClassOnHeap、AllocStructOnHeap、DeepCopyStruct）先查空闲表：有空闲槽位就复用（resize到正确大小，设置m_slotKinds/m_slotStructIdx）；否则emplace_back一个新槽位。

### GC触发流程

```text
OP_New / OP_AllocStruct → set m_gcPending = true
String allocation (mint / concatenation node) → set m_gcPending = true
                            ↓
Function entry (frame push) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
Thread intrinsic (spawn, after minting the context) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && (heap > threshold || strings > strThreshold)
                   → CollectGarbage()
```

安全点只在持有GIL的载体上触发；停泊中的上下文不在运行，无需配合即可被扫描。
