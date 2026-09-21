# 模块序列化

编译产出的模块以 `.nmod` 文件分发与加载：字符串常量、函数、struct
与 class 表按固定布局依次序列化，加载器按同一布局读回，并通过
minor 版本号实施语义下限。想直接查看某个 `.nmod` 的内容，可以用
[ndisasm](../cli-tools/ndisasm.md) 在命令行反汇编检查。

编译后的模块保存为 `.nmod` 文件，布局如下：

```text
"NLANGMOD"     magic (8 bytes)
uint16 majorVer = 1
uint16 minorVer = 11
string moduleName
string[] stringConstants
function[] functions
struct[] structs
class[] classes
```

**版本历史**：v1.11（泛型数组类型实参）——一次语义下限抬升，不是布
局变更：没有新增序列化字段，但泛型容器的数组类型元素
（`List<T[]>`、`Dict` 的键/值）现在以裸数组句柄流动、不做装箱，
`foreach` 循环变量在其上占用 GC 会追踪的 `RTK_Array` 局部槽位。旧
ncc 产出的 v1.10 模块会把这些元素装箱进 GC 永不追踪的基本类型槽位，因
此加载器直接拒绝 minor < 11——旧模块必须重新编译。
v1.10（数组字段类型标记）——一次语义下限抬升，不是布局变更：没有新增序
列化字段，但 array 型 struct/class 字段的 `fieldTypeKinds` 条目现在
存 `RTK_Array`（此前存的是元素 kind），GC 的字段追踪与 struct 流式
处理按该 kind 分派。旧 ncc 编译的 v1.9 模块携带旧语义，因此加载器
直接拒绝 minor < 10——旧模块必须重新编译。
v1.9（调试器）——每个函数记录以一个 `sourceFile` 字符串收尾
（编译所在翻译单元的路径，位于 locals 块之后）；导入合并同时拷贝
`func.locals`，被导入帧的 GC 根集因此完整。
v1.8——一等函数值：`RTK_Func` kind 字节加上前述 8 条函
数值指令；旧 VM 无法执行这些指令，会直接拒绝 v1.8 模块（下限在此步
升到 minor 8）。
v1.7——stdlib 命名空间内建函数 + 保留命名空间；字段布
局无变化，但关系比较字符串指令共用这一版本步进，旧 VM 必须拒绝这些
模块（下限在此步升到 minor 7）。

每个 struct 包含：name、fieldCount、fieldNames[]、fieldTypeKinds[]、
fieldStructIndices[]、fieldClassIndices[]。

每个 class 包含：name、fieldCount、superClassIdx、fieldNames[]、
fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、
fieldAccess[]、methodIndices[]、constructorIdx。
