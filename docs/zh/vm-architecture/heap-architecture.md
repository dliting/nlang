# 堆架构


### 单一堆设计

struct 与 class 对象共享 `m_structHeap: vector<StructSlot>`，其中
`StructSlot = vector<int32>`。这简化了分配，也让字段访问可以统一走
`OP_LoadField`/`OP_StoreField`。

### 对象布局

**struct**：`[field0, field1, ..., fieldN-1]`
- 没有类型头（靠 `m_slotStructIdx` 并行数组识别）
- 字段在字节码中的偏移 = field_index * 4

**class**：`[classIdx, field0, field1, ..., fieldN-1]`
- slot[0] = classIdx，用于运行期类型识别（虚分派）
- 字段在字节码中的偏移 = (field_index + 1) * 4（把 classIdx 算进去）
- 继承字段排在自身字段之前（根祖先在前）

### 并行数组

| 数组            | 用途                                       |
|-----------------|--------------------------------------------|
| m_slotKinds     | 每个堆（heap）槽位的 RTK_Class/RTK_Struct/RTK_Boxed/0=空闲 |
| m_slotStructIdx | 每个 struct 槽位对应的 CompiledStruct 索引 |
| m_markBits      | 每个堆槽位的 GC 标记位                     |

`m_slotStructIdx` 之所以存在，是因为 struct 对象没有类型头。缺少它，
MarkStruct 在追踪引用时无从得知该查询哪个 CompiledStruct 的字段布局。
替代方案（给 struct 对象加类型头）要求把所有 LoadField/StoreField 的
偏移整体 +1，改动面更大也更容易出错。

### 隐式 `Object` 基类（Phase 8e-1）

凡未显式继承其他类的用户类，都会由 `RegisterClasses` 的一个后置趟把
`superClassIdx` 设为合成 Object 类的索引。Object 是唯一
`superClassIdx == -1` 的类。

Object 有三个虚方法（`Equals(Object)→int`、`GetHashCode()→int`、
`toString()→string`），全部经由内建函数（intrinsic）分派：

| 内建函数 ID            | 行为                                           |
|------------------------|------------------------------------------------|
| INTR_Object_Equals    | 恒等比较：同一堆索引 → 1，否则 0（null==null→1） |
| INTR_Object_GetHashCode | 恒等：`this` 的堆索引（null→0）               |
| INTR_Object_toString  | `"ClassName@hex(heapIdx)"`（null→NPE）         |
| INTR_String_Equals    | 值比较：池内容相等                             |
| INTR_String_GetHashCode | 值：对内容做 `std::hash<std::string>`        |

既有的 `OP_CallMethod` 按名查找会先命中最派生的实现——Object 方法
没有单独的分派机制。当运行期走到 Object 的内建函数桩（不存在 AST
覆写）时，直接短路进入 `ExecuteIntrinsic`。

**两条内建函数分派路径（P3.2 修复）**：大多数内建函数经由
`OP_CallMethod` / `OP_CallMethodDirect` 到达，二者检查 VmBackend 盖在
`CompiledFunction` 上的 `callee.intrinsicId` 字段并短路进入
`ExecuteIntrinsic`。但 string 是基元类型（没有类），
`string.getHashCode()` 与 `string.equals()` 走不了这条路——VmBackend
为它们直接发射 `OP_CallIntrinsic`，执行器同样经 `ExecuteIntrinsic`
函数分派。（在 commit 862d7a7 之前，`OP_CallIntrinsic` 分支会抛
"intrinsic calls not yet implemented"；两个 e2e 测试之所以通过，只是
因为该抛错退出码恰好等于期望值。）

### 装箱基元（Phase 8e-1）

基元值（int/float/string）赋给 Object 类型的目标时，会装箱（box）成
一个 2 槽堆条目：

```text
slot[0] = 类型标签（RTK_Int32 / RTK_Float / RTK_String）
slot[1] = 值位（int32 / float 位型 / string 池索引）
```

`m_slotKinds[idx] = RTK_Boxed`（6）。GC MarkPhase 完全跳过 RTK_Boxed
槽位——它们不持有任何出边引用，遍历纯属浪费，还会把 slot[0] 里的
类型标签误读成 classIdx。SweepPhase 与其他不可达槽位一样释放它们。

### 内建泛型 `List<T>`（Phase 8e-3）

`List<T>` 是内建泛型类，采用**擦除语义**（Java 模型）：类型参数 `T`
只存在于编译期。运行期所有 `List<X>` 实例化共享同一个后备
CompiledClass（"List"），其上只有一个 int 类型的隐藏字段 `__handle`。

**编译期模型。** resolver 维护一份合成 SnClassDecl 缓存，键是完整的
实例化签名：

```cpp
struct GenericInstKey {
    std::string baseName;            // "List" / "Dict" / "Func"
    std::vector<SnField*> typeArgs;  // resolved type-argument fields
    std::vector<uint8> outFlags;     // Phase 13: out-marked params (Func)
    std::vector<uint8> arrayFlags;   // C-period: array-typed type args
};
```

每次缓存未命中都会铸造一个新的 SnClassDecl，例如名为 `"List<int>"`，
其方法携带**替换后的签名**（T → int / Point / ...）供静态类型检查
使用。一份多态签名登记表驱动替换：

| 方法       | 参数槽位       | 返回槽位    |
|------------|----------------|-------------|
| Add        | [T]            | void        |
| Get        | [int]          | T           |
| Set        | [int, T]       | void        |
| Length     | []             | int         |
| RemoveAt   | [int]          | void        |
| IndexOf    | [T]            | int         |
| Contains   | [T]            | int         |
| Clear      | []             | void        |

SnClassDecl 上的 `m_bIsGenericInst = true` 标志标记合成实例，代码生
成据此为基元 T 的方法参数发射 `OP_Box`、为基元 T 的 Get() 返回值发射
`OP_Unbox`——**类型实参是数组类型时除外**（`List<int[]>`）：擦除后的
T 是基元 kind，但元素以裸数组句柄流动、不做装箱；逐参数的数组性随
实例化本身传递（`GenericArrayFlags()`），所有装箱区域与 resolver 门
都从这里读取。

**运行期存储。** List 元素存放在侧表中：

```cpp
struct ListSlot {
    std::vector<int32_t> elements;   // heap idxs (boxed primitives, class refs, or raw array handles)
};

std::vector<ListSlot>   m_listStore;       // index = __handle - 1 (0 reserved for null)
std::vector<int32_t>    m_listFreeList;    // recycled slots after GC sweep
```

所有元素统一都是堆索引——基元 T 的值在调用点装箱（`OP_CallMethod`
之前先 `OP_Box typeKind`），class-T 与数组 T 的值原样通过。GC 在追踪
时确实会按每个元素的运行期槽位 kind 分派：引用 kind 的元素
（class/struct/array/func）被标记并压入工作表，其子节点——class 字段
或（对数组元素而言）按 elemKind 的数组自身元素——也随之被追踪。

**内建函数（新增 9 个 ID）。** List 方法与普通类方法一样经
`OP_CallMethod` 分派，但每个名字背后的函数都是一个触发
`ExecuteIntrinsic` 的空桩：

| 内建函数 ID             | 行为                                                    |
|-------------------------|---------------------------------------------------------|
| `INTR_List_Ctor`        | 分配 ListSlot，把索引存入 `this.__handle`               |
| `INTR_List_Add`         | 从参数读取值的堆索引（装箱基元或裸句柄），push_back    |
| `INTR_List_Get`         | 读取 int 索引，返回 elements[idx]                       |
| `INTR_List_Set`         | 读取 int 索引 + 堆索引，替换 elements[idx]              |
| `INTR_List_Length`      | 返回 elements.size()                                    |
| `INTR_List_RemoveAt`    | 读取 int 索引，elements.erase(...)                      |
| `INTR_List_IndexOf`     | 线性查找；返回位置或 -1                                 |
| `INTR_List_Contains`    | 线性查找；返回 1 / 0                                    |
| `INTR_List_Clear`       | elements.clear()                                        |

**GC 集成。** MarkPhase 正常遍历 class 实例的子节点；遇到 slot[0]
（`classIdx`）等于缓存的 `m_listClassIdx` 的实例时，额外读取
`__handle`（slot[1]），并把 `m_listStore[__handle-1].elements` 的每
个条目按堆引用标记，把引用 kind 的条目（class/struct/array/func）压
入工作表使其子节点也被追踪——从这里压入的数组元素会进入工作表的
`RTK_Array` 分支，按 elemKind 追踪数组自身的元素（`List<Point[]>`
就是这样让 `Point` 记录存活的）。越界与空闲索引直接跳过。

SweepPhase 与之镜像：List 实例被回收时，其 `__handle` 被压入
`m_listFreeList`，供下一次 `INTR_List_Ctor` 复用。ListSlot 本身不释
放（句柄复用只发生在 *这个* List 死亡之后，实际上不可能再有来自其他
List 实例的存活引用——该槽位内容已不可达）。

**代码生成：NewExpr 的构造函数别名修复。** 当 `new T(args)` 作为方法
调用实参出现时，朴素的发射顺序（先把构造实参求值进 callParamBase，
再 OP_New 写入 resultOffset）会覆盖同一个 callParamBase 槽位。修复：
求值完构造实参后，把 OP_New 的结果分配到最后一个参数*之后*的槽位
（`callParamBase + paramIdx * VALUE_SIZE`），再拷贝到 resultOffset：

```text
// args evaluated into callParamBase[1..N]
allocSlot = callParamBase + N * VALUE_SIZE
OP_New allocSlot, classIdx
OP_CallMethodDirect ctorIdx, callParamBase   // ctor reads this=allocSlot
OP_VarLocal allocSlot
OP_Assign resultOffset                        // copy to caller's expected slot
```

### 内建泛型 `Dict<K,V>`（Phase 8e-4）

`Dict<K,V>` 沿用 `List<T>` 的架构：**擦除语义**、所有实例化共享单一
后备 CompiledClass（"Dict"）、一个 int 类型的隐藏字段 `__handle`。
编译期缓存键变为 `(baseName, typeArgs)`，其中 `typeArgs.size()` == 2；
运行期类无论 K、V 如何都共享。

**运行期存储。** 条目存放在侧表中：

```cpp
struct DictSlot {
    std::vector<std::pair<int32_t,int32_t>> entries;  // (K heap idx, V heap idx)
};

std::vector<DictSlot>   m_dictStore;       // index = __handle - 1
std::vector<int32_t>    m_dictFreeList;    // recycled slots after GC sweep
int16_t                 m_dictClassIdx;    // cached at module load
```

K 与 V 同样统一都是堆索引——基元 K/V 的值在调用点装箱（
`OP_CallMethod` 之前先 `OP_Box typeKind`）。查找是**线性扫描** O(n)；
开放寻址哈希表优化留待后续阶段。

**内建函数（新增 7 个 ID 53-59）。**

| 内建函数 ID           | 行为                                                      |
|------------------------|-----------------------------------------------------------|
| `INTR_Dict_Ctor`       | 分配 DictSlot，把索引存入 `this.__handle`                 |
| `INTR_Dict_Set`        | 线性扫描；键已存在则替换 V，否则追加 (K,V)                |
| `INTR_Dict_Get`        | 线性扫描；不存在则抛 "Dict key not found"                 |
| `INTR_Dict_ContainsKey`| 线性扫描；返回 1 / 0                                      |
| `INTR_Dict_Remove`     | 线性扫描；找到则擦除，返回 1 / 0                          |
| `INTR_Dict_Clear`      | entries.clear()                                           |
| `INTR_Dict_Count`      | 返回 entries.size()                                       |

**kind 感知的相等判断。** `DictKeysEqual(k1, k2)` 是 Set/Get/
ContainsKey/Remove 共用的核心辅助函数：

1. 恒等快路径（`k1 == k2`）。
2. 越界与 kind 匹配检查（`m_slotKinds[k1] == m_slotKinds[k2]`）。
3. 按 kind 分支：
   - `RTK_Class` / `RTK_Struct`：恒等比较（比较堆索引）。
   - `RTK_Boxed`：按内部类型标签分支（`m_structHeap[k][0]`）：
     - `RTK_Int32` / `RTK_Float`：比较
       `m_structHeap[k][kBoxedValueSlot]` 处的值位（IEEE 754——
       `NaN != NaN`）。
     - `RTK_String`：比较 `m_stringPool[bits]` 的内容（值相等）。

这一模式把 Phase 8e-3 的补救修复 C2（List IndexOf/Contains 的值位比
较）推广到多标签键。

**代码生成：逐方法装箱计划。** 取代 List 专属的代码生成块。当调用
目标的类是泛型实例化（`SnClassDecl::IsGenericInstantiation()`）时，
代码生成基于 `(baseName, methodName, typeArgs)` 构建逐方法计划：

```cpp
struct ArgBoxPlan { uint8_t tag; bool needsBox; };
std::map<uint16_t, ArgBoxPlan> argPlans;   // paramIdx -> plan
bool    returnsBoxed = false;
uint8_t returnTag    = 0;
```

- List 分派：`Add` → 槽 1；`Set` → 槽 2；`IndexOf`/`Contains`
  → 槽 1；`Get` → returnsBoxed。
- Dict 分派：`Set` → K 在槽 1 + V 在槽 2；`Get` → K 在槽
  1 + returnsBoxed（V）；`ContainsKey`/`Remove` → K 在槽 1。

共享辅助函数 `BoxingTagFor(SnField*)` 返回
`BoxingTagResult {tag, isPrimitive}`，从此 `RTK_Int32 == 0` 不再与
"无装箱" 冲突——`isPrimitive` 布尔值才是权威信号。参数循环对
`argPlans` 中的每个槽位在 `OP_CallMethod` 之前发射 `OP_Box <tag>`；
调用之后若 `returnsBoxed` 则发射 `OP_Unbox <tag>`。

**GC 集成。** MarkPhase：当 class 实例的 slot[0] 等于
`m_dictClassIdx` 时，读取 `__handle`（slot[1]），把每个条目的 K 与 V
都按堆引用标记。越界 / 空闲索引跳过。SweepPhase：Dict 实例被回收时，
其 `__handle` 压入 `m_dictFreeList`，供下一次 `INTR_Dict_Ctor` 复用。

**ReadHandle 统一校验。** `ReadDictHandle(callParamBase, locals,
methodName)` 与 `ReadListHandle` 镜像：从参数基址读取 `thisHeapIdx`，
`thisHeapIdx <= 0` 抛 "Dict `<method>` on null instance"，超过堆大小
抛 "...on stale reference"，句柄槽位为 0 抛 "...on uninitialized
instance"。全部 6 个构造后内建函数都经由这个辅助函数。
