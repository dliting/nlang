# 内存管理


### 堆布局

所有 struct 与 class 对象共享单一堆（`m_structHeap`）。每个堆槽位是
一个 `vector<int32>`：

- **struct 槽位**：`[field0, field1, ...]`（没有类型头）
- **class 槽位**：`[classIdx, field0, field1, ...]`（slot[0] = 运行期
  类型 ID）

堆索引 0 是哨兵（null/无效）。有效索引从 1 开始。

### 垃圾回收

NLang 对堆记录（class、struct、数组与函数值）使用标记-清扫垃圾
回收器：

1. **触发**：GC 在安全点运行，条件是 `m_gcPending` 已置位且堆大小
   超过阈值。安全点是函数入口与循环回边。
2. **标记阶段**：经 `LocalDescriptor` 精确扫描——声明 kind 为引用
   kind 的局部槽位（及进行中的返回值槽位）会被扫描——class、struct、
   数组或函数记录；基本类型永不扫描。没有保守字节扫描（与栈帧物理
   布局解耦）。标记使用迭代式工作列表（非递归），避免深对象链上的
   栈溢出。
3. **清扫阶段**：未标记的引用记录（class、struct、数组、函数）被
   释放。class 持有的 struct 字段（值语义）随其宿主 class 一起释放。
   class 字段（引用语义）不可达时由 GC 独立释放。
4. **空闲表**：被清扫的槽位进入空闲表。新分配优先复用空闲槽位。

**设计决策**（记录于 VmExecutor.h）：
- 由安全点触发，而非由分配点触发（避免在 MarkPhase 中追踪
  tempSlot/tempSlot2）
- 经 LocalDescriptor 精确扫描，而非保守字节扫描（把 GC 与栈帧物理
  布局解耦）
- `m_slotStructIdx` 并行数组识别 struct 类型（struct 没有类型头；
  比添加类型头改动更小）
- 迭代式标记 + 工作列表，而非递归（深对象链会溢出 C++ 调用栈；
  内存用量以 O(可达对象数) 为界）

### struct 生命周期

struct 的生命周期与宿主绑定（值语义）：
- 局部 struct 变量：存活到函数返回（或变量被重新赋值——届时旧
  struct 持有的嵌套 struct 由深拷贝逻辑释放）
- class 持有的 struct 字段：宿主 class 对象被 GC 清扫时释放
- struct 持有的嵌套 struct：随父级递归释放
