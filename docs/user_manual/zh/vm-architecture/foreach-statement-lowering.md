# foreach语句降级

`foreach`让开发者不手动管理下标就能遍历数组、集合与字符串。VM层面它没有专用指令（字符串一路除外，见下）：编译器把`foreach (Type var in iterable) { body }`为Array/List/Dict编译为**基于索引的展开**——与`for`循环同形，外加一个把元素`i`装入用户变量槽位的循环体前奏——为字符串编译为专用的码点展开。

### 隐藏局部变量（为嵌套而唯一化）

每个基于索引的`foreach`在`LoopContext`入栈之前分配四个隐藏局部变量，名字带一个逐函数计数器，避免嵌套循环撞上`AllocLocal`的按名称去重：

| 局部变量 | typeKind | 用途 |
|-------|----------|---------|
| `<varName>` | 由元素类型推导——元素为数组类型时是`RTK_Array` | 用户可见的循环变量 |
| `__foreach_iter_<N>` | `RTK_Array`（数组）或`RTK_Class`（List/Dict） | 可迭代对象引用 |
| `__foreach_i_<N>` | `RTK_Int32` | 循环计数器 |
| `__foreach_n_<N>` | `RTK_Int32` | 缓存的长度 |

`<N>`来自`FuncContext::foreachCounter`，它在函数入口重置为0。字符串一路改分配另一组三件套：`__foreach_iter_<N>`（`RTK_String`）、`__foreach_off_<N>`（`RTK_Int32`，字节偏移）与`__foreach_cond_<N>`（`RTK_Int32`，继续标志），循环变量按`RTK_Char`分配。

### 代码生成四分支

可迭代对象的kind在代码生成期（而非resolver期）分类，保持用户可见的抽象语法树（AST，abstract syntax tree）不变：

- **数组**（`T[N]`）：由两个探测臂识别——声明侧臂（`Field`带`IsArrayType()`的标识符或成员左值）与值形态臂（`EvalDataType()`携带驻留数组类型记号的任意表达式：`List<T[]>`/`Dict<K[],V>`的`get()`与下标读取、返回数组的调用、新建数组表达式）。长度走`OP_ArrayLength`；元素走`OP_LoadElement`（struct元素类型另加`OP_CopyStruct`，读取时值语义深拷贝）。
- **List<T>**：可迭代对象的`EvalDataType()`是满足`BaseName()=="List"`且`IsGenericInstantiation()`的`SnClassDecl`。长度走`OP_CallMethod "length"`；元素走`OP_CallMethod "get"`，基本类型T后接`OP_Unbox`——类型实参是数组类型时跳过（`List<int[]>`的元素以裸句柄流动；逐方法装箱计划的数组实参例外）。
- **Dict<K,V>**：可迭代对象的`EvalDataType()`是满足`BaseName()=="Dict"`的`SnClassDecl`。**内联`keys()`调用**先把一个新`List<K>`物化进`iterSlot`，其余照搬List路径、元素类型为K。
- **字符串**：按Unicode码点迭代。循环头本身就是步进——一条`OP_StrForeachStep`把字节偏移处的码点解码进循环变量、按编码长度推进偏移、写出继续标志；对该标志的`OP_JumpIfNot`结束循环。`continue`跳回头部（步进即增量），`break`跳到循环末尾。没有空检查：空源按空串读取，第一步即清除标志。

每个隐藏局部变量的`typeKind`正是垃圾回收（GC，garbage collection）在安全点识别引用根的依据，因此数组路径的`iterSlot`必须是`RTK_Array`、List/Dict必须是`RTK_Class`——标签打错要么泄漏引用（漏掉根），要么把整数槽位当堆索引误追踪。用户可见的循环变量同理：数组类型的循环变量（`foreach (int[] row in grid)`）的槽位按`RTK_Array`分配，每轮迭代绑定的句柄因此是被追踪的根。

### `Dict.keys()`内建函数

`INTR_Dict_Keys = 60`（include/nlang/vm/Intrinsics.h）。以名为`keys`的方法注册在Dict类上，`paramCount=1, returnTypeKind=RTK_Class`。VmExecutor处理器：

1. 经`ReadDictHandle`读取dict句柄（统一的null/stale/uninit校验）。
2. `AllocClassOnHeap(m_listClassIdx)`——新的List类实例。
3. `AllocListHandle()`——新的List侧表槽位。
4. 把句柄接进`m_structHeap[heapIdx][kListHandleFieldOffset]`。
5. 把`dict.entries[i].first`（K的堆索引）拷进`m_listStore[handle-1].elements`。
6. 把`listHeapIdx`写入`pResult`；置`m_gcPending = true`。

产出的`List<K>`由既有GC `MarkPhase`自动追踪（它本来就处理`classIdx == m_listClassIdx`）。K的堆索引已存在于dict中并经其追踪；新List持有的是同一批对象的额外引用，这是安全的（GC标记位自然去重）。

### LoopContext复用

`m_loopStack`已经为`for`/`while`/`do`与`switch`支持`break`/`continue`。`foreach`压入一个`LoopContext{isSwitch=false}`并复用同一套break/continue回填逻辑。基于索引的展开中`continue`跳到循环体后的"i = i + 1"处；字符串展开中它跳回头部（步进推进偏移）。`break`在两路中都跳到循环末尾。
