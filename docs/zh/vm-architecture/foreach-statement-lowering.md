# foreach 语句降级

`foreach` 让开发者不手动管理下标就能遍历数组与集合。VM 层面它没有
专用指令：编译器把 `foreach (Type var in iterable) { body }` 编译为
**基于索引的展开**——不引入新指令。12 步降级沿用 `for` 循环的模式
（`VmBackend.cpp` 的 `NK_ForStmt` 路径），外加一个把元素 `i` 装入用
户变量槽位的循环体前奏。

### 隐藏局部变量（为嵌套而唯一化）

每个 `foreach` 在 `LoopContext` 入栈之前分配四个隐藏局部变量，名字
带一个逐函数计数器，避免嵌套循环撞上 `AllocLocal` 的按名去重：

| 局部变量 | typeKind | 用途 |
|-------|----------|---------|
| `<varName>` | 由元素类型推导——元素为数组类型时是 `RTK_Array` | 用户可见的循环变量 |
| `__foreach_iter_<N>` | `RTK_Array`（数组）或 `RTK_Class`（List/Dict） | 可迭代对象引用 |
| `__foreach_i_<N>` | `RTK_Int32` | 循环计数器 |
| `__foreach_n_<N>` | `RTK_Int32` | 缓存的长度 |

`<N>` 来自 `FuncContext::foreachCounter`，它在函数入口重置为 0。

### 代码生成三分支

可迭代对象的 kind 在代码生成期（而非 resolver 期）检测，保持用户可
见的 AST 不变：

- **数组**（`T[N]`）：可迭代对象是 `Field` 带 `IsArrayType()` 的
  `SnIdentifierExpr`。长度走 `OP_ArrayLength`；元素走
  `OP_LoadElement`（struct 元素类型另加 `OP_CopyStruct`）。
- **List<T>**：可迭代对象的 `EvalDataType()` 是满足
  `BaseName()=="List"` 且 `IsGenericInstantiation()` 的
  `SnClassDecl`。长度走 `OP_CallMethod "length"`；元素走
  `OP_CallMethod "get"`，基本类型 T 后接 `OP_Unbox`——类型实参是数组类
  型时跳过（`List<int[]>` 的元素以裸句柄流动；逐方法装箱计划的数组
  实参例外）。
- **Dict<K,V>**：可迭代对象的 `EvalDataType()` 是满足
  `BaseName()=="Dict"` 的 `SnClassDecl`。**内联 `keys()` 调用**先把
  一个新 `List<K>` 物化进 `iterSlot`（步骤 2b），其余照搬 List 路
  径、元素类型为 K。

每个隐藏局部变量的 `typeKind` 正是 GC 在安全点识别引用根的依据，因
此数组路径的 `iterSlot` 必须是 `RTK_Array`、List/Dict 必须是
`RTK_Class`——标签打错要么泄漏引用（漏掉根），要么把整数槽位当堆索
引误追踪。用户可见的循环变量同理：数组类型的循环变量
（`foreach (int[] row in grid)`）的槽位按 `RTK_Array` 分配，每轮迭
代绑定的句柄因此是被追踪的根。

### `Dict.keys()` 内建函数

`INTR_Dict_Keys = 60`（CompiledModule.h）。以 `paramCount=1,
returnTypeKind=RTK_Class` 注册为 Dict 类的方法。VmExecutor 处理器：

1. 经 `ReadDictHandle` 读取 dict 句柄（统一的 null/stale/uninit 校
   验）。
2. `AllocClassOnHeap(m_listClassIdx)`——新的 List 类实例。
3. `AllocListHandle()`——新的 List 侧表槽位。
4. 把句柄接进 `m_structHeap[heapIdx][kListHandleFieldOffset]`。
5. 把 `dict.entries[i].first`（K 的堆索引）拷进
   `m_listStore[handle-1].elements`。
6. 把 `listHeapIdx` 写入 `pResult`；置 `m_gcPending = true`。

产出的 `List<K>` 由既有 GC `MarkPhase` 自动追踪（它本来就处理
`classIdx == m_listClassIdx`）。K 的堆索引已存在于 dict 中并经其追
踪；新 List 持有的是同一批对象的额外引用，这是安全的（GC 标记位自
然去重）。

### LoopContext 复用

`m_loopStack` 已经为 `for`/`while`/`do` 与 `switch` 支持
`break`/`continue`。`foreach` 压入一个 `LoopContext{isSwitch=false}`
并复用同一套 break/continue 回填逻辑。`continue` 跳到循环体后的
"i = i + 1" 处；`break` 跳到循环末尾。
