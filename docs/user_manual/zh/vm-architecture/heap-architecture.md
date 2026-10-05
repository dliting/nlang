# 堆架构

class、struct、装箱基本类型、函数值与数组记录共享同一个堆数组（`m_structHeap`），字符串对象存放在独立的对象仓（`m_stringObjs`），两者都由垃圾回收器统一回收。本页说明堆的物理布局——槽位结构、并行数组、字符串对象仓——以及内建泛型容器`List<T>`/`Dict<K,V>`的存储方式，是理解[垃圾回收设计](garbage-collection-design.md)页的前提。

### 单一堆设计

struct与class对象共享`m_structHeap: vector<StructSlot>`，其中`StructSlot = vector<int32>`。这简化了分配，也让字段访问可以统一走`OP_LoadField`/`OP_StoreField`。

### 对象布局

**struct**：`[field0, field1, ..., fieldN-1]`
- 没有类型头（靠`m_slotStructIdx`并行数组识别）
- 字段在字节码中的偏移 = field_index * 4

**class**：`[classIdx, field0, field1, ..., fieldN-1]`
- slot[0] = classIdx，用于运行期类型识别（虚分派）
- 字段在字节码中的偏移 = (field_index + 1) * 4（把classIdx算进去）
- 继承字段排在自身字段之前（根祖先在前）

### 并行数组

| 数组            | 用途                                       |
|-----------------|--------------------------------------------|
| m_slotKinds     | 每个堆（heap）槽位的RTK_Class/RTK_Struct/RTK_Boxed/0=空闲 |
| m_slotStructIdx | 每个struct槽位对应的CompiledStruct索引 |
| m_markBits      | 每个堆槽位的GC标记位                     |

`m_slotStructIdx`之所以存在，是因为struct对象没有类型头。缺少它，MarkStruct在追踪引用时无从得知该查询哪个CompiledStruct的字段布局。替代方案（给struct对象加类型头）要求把所有LoadField/StoreField的偏移整体+1，改动面更大也更容易出错。

### 隐式`Object`基类

凡未显式继承其他类的用户类，都会由`RegisterClasses`的一个后置趟把`superClassIdx`设为合成Object类的索引。Object是唯一`superClassIdx == -1`的类。

Object有三个虚方法（`equals(Object)→int`、`getHashCode()→int`、`toString()→string`），全部经由内建函数（intrinsic）分派：

| 内建函数标识符（ID，identifier）            | 行为                                           |
|------------------------|------------------------------------------------|
| INTR_Object_Equals    | 恒等比较：同一堆索引 → 1，否则0（null==null→1） |
| INTR_Object_GetHashCode | 恒等：`this`的堆索引（null→0）               |
| INTR_Object_toString  | `"ClassName@hex(heapIdx)"`（null→空指针异常（NPE，null pointer exception））         |
| INTR_String_Equals    | 值比较：内容相等                               |
| INTR_String_GetHashCode | 值：对内容做`std::hash<std::string>`        |

既有的`OP_CallMethod`按方法名查找会先命中最派生的实现——Object方法没有单独的分派机制。当运行期走到不存在抽象语法树（AST，abstract syntax tree）覆写的Object内建函数桩时，直接短路进入`ExecuteIntrinsic`。

**两条内建函数分派路径**：大多数内建函数经由`OP_CallMethod` / `OP_CallMethodDirect`到达，二者检查VmBackend盖在`CompiledFunction`上的`callee.intrinsicId`字段并短路进入`ExecuteIntrinsic`。但string是基本类型（没有类），`string.getHashCode()`与`string.equals()`走不了这条路——VmBackend为它们直接发射`OP_CallIntrinsic`，执行器同样经`ExecuteIntrinsic`函数分派。

### 装箱基本类型

基本类型值（int/float/string）赋给Object类型的目标时，会装箱（box）成一个2槽堆条目：

```text
slot[0] = 类型标签（RTK_Int32 / RTK_Float / RTK_String）
slot[1] = 值位（int32 / float 位模式 / string 对象句柄）
```

`m_slotKinds[idx] = RTK_Boxed`（6）。GC MarkPhase对装箱记录按类型标签分派：字符串标签的payload（slot[1]）是字符串对象仓的句柄，按句柄标记字符串对象；其余标签的值位是原始位模式，没有出边可追。slot[0]始终按类型标签读取，不会被误读成classIdx。SweepPhase与其他不可达槽位一样释放它们。

### 字符串对象仓

字符串在运行期是GC管理的不可变对象，存放在独立的对象仓`m_stringObjs`中。前五类堆记录（class/struct/boxed/func/array）共享`m_structHeap`，字符串是第六类存储，拥有自己的标记位向量、空闲表与回收阈值（回收语义见[垃圾回收设计](garbage-collection-design.md)页）。

- **句柄语义**：`RTK_String`槽位持有1-based句柄（0 = null，读作空串——与堆索引0的哨兵约定同形）。
- **Flat / Cons双形态**：对象要么是Flat（`str`持有内容），要么是Cons（`left`/`right`持有两个子节点）。拼接（`OP_Concat_str`）分配Cons节点，O(1)零拷贝；首次读取时就地展平为Flat——所有既有句柄看到的都是展平后的内容。
- **immortal与interned位**：`immortal`位标记随模块执行物化的常量对象（永不回收）；`interned`位标记进入短串驻留表（≤40字节按内容共享，内容到存活句柄的弱映射）的对象——两个驻留串的`==`等价于句柄比较。

### 内建泛型`List<T>`

`List<T>`是内建泛型类，采用**擦除语义**（Java模型）：类型参数`T`只存在于编译期。运行期所有`List<X>`实例化共享同一个后备CompiledClass（"List"），其上只有一个int类型的隐藏字段`__handle`。

**编译期模型。** resolver维护一份合成SnClassDecl缓存，键是完整的实例化签名：

```cpp
struct GenericInstKey {
    std::string baseName;            // "List" / "Dict" / "Func"
    std::vector<SnField*> typeArgs;  // resolved type-argument fields
    std::vector<uint8> outFlags;     // out-marked params (Func)
    std::vector<uint8> arrayFlags;   // array-typed type args
};
```

每次缓存未命中都会铸造一个新的SnClassDecl，例如名为`"List<int>"`，其方法携带**替换后的签名**（T → int / Point / ...）供静态类型检查使用。一份多态签名登记表驱动替换：

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

SnClassDecl上的`m_bIsGenericInst = true`标志标记合成实例，代码生成据此为基本类型T的方法参数发射`OP_Box`、为基本类型T的Get()返回值发射`OP_Unbox`——**类型实参是数组类型时除外**（`List<int[]>`）：数组类型的实参以驻留的数组类型令牌保存在实例化的类型实参槽中，其运行期kind是`RTK_Array`（非基本类型），装箱区域自然将这类元素按裸数组句柄处理、不做装箱。

**运行期存储。** List元素存放在侧表中：

```cpp
struct ListSlot {
    std::vector<int32_t> elements;   // heap idxs (boxed primitives, class refs, or raw array handles)
};

std::vector<ListSlot>   m_listStore;       // index = __handle - 1 (0 reserved for null)
std::vector<int32_t>    m_listFreeList;    // recycled slots after GC sweep
```

所有元素统一都是堆索引——基本类型T的值在调用点装箱（`OP_CallMethod`之前先`OP_Box typeKind`），class-T与数组T的值原样通过。GC在追踪时确实会按每个元素的运行期槽位kind分派：引用kind或装箱记录的元素（class/struct/boxed/array/func）被标记并压入工作列表，其子节点——class字段、装箱串的payload，或（对数组元素而言）按elemKind的数组自身元素——也随之被追踪。

**内建函数ID（9个）。** List方法与普通类方法一样经`OP_CallMethod`分派，但每个名字背后的函数都是一个触发`ExecuteIntrinsic`的空桩：

| 内建函数ID             | 行为                                                    |
|-------------------------|---------------------------------------------------------|
| `INTR_List_Ctor`        | 分配ListSlot，把索引存入`this.__handle`               |
| `INTR_List_Add`         | 从参数读取值的堆索引（装箱的基本类型值或裸句柄），push_back    |
| `INTR_List_Get`         | 读取int索引，返回elements[idx]                       |
| `INTR_List_Set`         | 读取int索引 + 堆索引，替换elements[idx]              |
| `INTR_List_Length`      | 返回elements.size()                                    |
| `INTR_List_RemoveAt`    | 读取int索引，elements.erase(...)                      |
| `INTR_List_IndexOf`     | 线性查找；返回位置或 -1                                 |
| `INTR_List_Contains`    | 线性查找；返回1 / 0                                    |
| `INTR_List_Clear`       | elements.clear()                                        |

**GC集成。** MarkPhase正常遍历class实例的子节点；遇到slot[0]（`classIdx`）等于缓存的`m_listClassIdx`的实例时，额外读取`__handle`（slot[1]），并把`m_listStore[__handle-1].elements`的每个条目按堆引用标记，把引用kind或装箱记录的条目（class/struct/boxed/array/func）压入工作列表使其子节点也被追踪——从这里压入的数组元素会进入工作列表的`RTK_Array`分支，按elemKind追踪数组自身的元素（`List<Point[]>`就是这样让`Point`记录存活的）。越界与空闲索引直接跳过。

SweepPhase与之镜像：List实例被回收时，其`__handle`被压入`m_listFreeList`，供下一次`INTR_List_Ctor`复用。ListSlot本身不释放（句柄复用只发生在*这个* List死亡之后，实际上不可能再有来自其他List实例的存活引用——该槽位内容已不可达）。

**代码生成：NewExpr的构造函数别名修复。**当`new T(args)`作为方法调用实参出现时，朴素的发射顺序（先把构造实参求值进callParamBase，再OP_New写入resultOffset）会覆盖同一个callParamBase槽位。修复：求值完构造实参后，把OP_New的结果分配到最后一个参数*之后*的槽位（`callParamBase + paramIdx * VALUE_SIZE`），再拷贝到resultOffset：

```text
// args evaluated into callParamBase[1..N]
allocSlot = callParamBase + N * VALUE_SIZE
OP_New allocSlot, classIdx
OP_CallMethodDirect ctorIdx, callParamBase   // ctor reads this=allocSlot
OP_VarLocal allocSlot
OP_Assign resultOffset                        // copy to caller's expected slot
```

### 内建泛型`Dict<K,V>`

`Dict<K,V>`沿用`List<T>`的架构：**擦除语义**、所有实例化共享单一后备CompiledClass（"Dict"）、一个int类型的隐藏字段`__handle`。编译期缓存键变为`(baseName, typeArgs)`，其中`typeArgs.size()` == 2；运行期类无论K、V如何都共享。

**运行期存储。**条目存放在侧表中：

```cpp
struct DictSlot {
    std::vector<std::pair<int32_t,int32_t>> entries;  // (K heap idx, V heap idx)
};

std::vector<DictSlot>   m_dictStore;       // index = __handle - 1
std::vector<int32_t>    m_dictFreeList;    // recycled slots after GC sweep
int16_t                 m_dictClassIdx;    // cached at module load
```

K与V同样统一都是堆索引——基本类型K/V的值在调用点装箱（`OP_CallMethod`之前先`OP_Box typeKind`）。查找是**线性扫描** O(n)。

**内建函数ID（53-59，共7个）。**

| 内建函数ID           | 行为                                                      |
|------------------------|-----------------------------------------------------------|
| `INTR_Dict_Ctor`       | 分配DictSlot，把索引存入`this.__handle`                 |
| `INTR_Dict_Set`        | 线性扫描；键已存在则替换V，否则追加(K,V)                |
| `INTR_Dict_Get`        | 线性扫描；不存在则抛 "Dict key not found"                 |
| `INTR_Dict_ContainsKey`| 线性扫描；返回1 / 0                                      |
| `INTR_Dict_Remove`     | 线性扫描；找到则擦除，返回1 / 0                          |
| `INTR_Dict_Clear`      | entries.clear()                                           |
| `INTR_Dict_Count`      | 返回entries.size()                                       |

**kind感知的相等判断。** `DictKeysEqual(k1, k2)`是Set/Get/
ContainsKey/Remove共用的核心辅助函数：

1. 恒等快路径（`k1 == k2`）。
2. 越界与kind匹配检查（`m_slotKinds[k1] == m_slotKinds[k2]`）。
3. 按kind分支：
   - `RTK_Class` / `RTK_Struct`：恒等比较（比较堆索引）。
   - `RTK_Boxed`：按内部类型标签分支（`m_structHeap[k][0]`）：
     - `RTK_Int32` / `RTK_Float`：比较`m_structHeap[k][kBoxedValueSlot]`处的值位（IEEE 754——
       `NaN != NaN`）。
     - `RTK_String`：比较句柄对应的字符串对象内容（值相等）。

这一模式把List `IndexOf`/`Contains`的值位比较推广到多标签键。

**代码生成：逐方法装箱计划。**取代List专属的代码生成块。当调用目标的类是泛型实例化（`SnClassDecl::IsGenericInstantiation()`）时，代码生成基于`(baseName, methodName, typeArgs)`构建逐方法计划：

```cpp
struct ArgBoxPlan { uint8_t tag; bool needsBox; };
std::map<uint16_t, ArgBoxPlan> argPlans;   // paramIdx -> plan
bool    returnsBoxed = false;
uint8_t returnTag    = 0;
```

- List分派：`add` → 槽1；`set` → 槽2；`indexOf`/`contains`
  → 槽1；`get` → returnsBoxed。
- Dict分派：`set` → K在槽1 + V在槽2；`get` → K在槽1 + returnsBoxed（V）；`containsKey`/`remove` → K在槽1。

共享辅助函数`BoxingTagFor(SnField*)`返回`BoxingTagResult {tag, isPrimitive}`，因而`RTK_Int32 == 0`不再与「无装箱」冲突——`isPrimitive`布尔值才是权威信号。参数循环对`argPlans`中的每个槽位在`OP_CallMethod`之前发射`OP_Box <tag>`；调用之后若`returnsBoxed`则发射`OP_Unbox <tag>`。

**GC集成。** MarkPhase：当class实例的slot[0]等于`m_dictClassIdx`时，读取`__handle`（slot[1]），把每个条目的K与V都按堆引用标记。越界 / 空闲索引跳过。SweepPhase：Dict实例被回收时，其`__handle`压入`m_dictFreeList`，供下一次`INTR_Dict_Ctor`复用。

**ReadHandle统一校验。** `ReadDictHandle(callParamBase, locals,
methodName)`与`ReadListHandle`镜像：从参数基址读取`thisHeapIdx`，`thisHeapIdx <= 0`抛 "Dict `<method>` on null instance"，超过堆大小抛 "...on stale reference"，句柄槽位为0抛 "...on uninitialized
instance"。全部6个构造后内建函数都经由这个辅助函数。
