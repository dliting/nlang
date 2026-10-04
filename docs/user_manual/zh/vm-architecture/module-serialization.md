# 模块序列化

编译产出的单元映像以 `.ncu` 文件分发与加载：字符串常量、函数、struct
与 class 表按固定布局依次序列化，加载器按同一布局读回，并通过
major/minor 版本号实施格式地板。多单元的程序与库打成 `.npkg` 包归档
（见文末）。想直接查看某个产物的内容，可以用
[ndisasm](../cli-tools/ndisasm.md) 在命令行反汇编检查。

编译后的单元映像保存为 `.ncu` 文件，布局如下：

```text
"NLANGCU "    magic (8 bytes, NUL-padded)
uint16 majorVer = 2
uint16 minorVer = 1
string modulePath        v2.0：本单元的点分模块路径（包身份）
string moduleName
string entryKey          v2.0：入口函数的限定名；空 = 本单元无入口
string[] stringConstants
function[] functions
struct[] structs
class[] classes
arrayType[] arrayTypes
enumNames[] / enumKeys[] 并行的两张枚举表
importSlots × 4          v2.0：function/class/struct/enum 四张导入槽表
```

四张导入槽表各为一列「模块路径 + 名字 + 形参数」条目（函数表另带
ownerClassKey，空 = 命名空间级函数）。自有条目占用各表前缀下标
0..n-1，导入槽追加在其后（n..n+m）——跨单元调用的操作数与自有调用
共用同一个下标空间，链接器对全部操作数统一重映射，无需区分操作码。
入口不再序列化为索引：程序包的入口记录（`.npkg` 头部）或裸单元的
`<modulePath>.main` 约定，在加载期按限定名解析成表下标。

**版本历史**：v2.1（并行开发线合并）——布局不变：并行线上的内容级
变更——12 基元标量 kind 表与逐局部变量的声明作用域字段——并入 v2.0
布局。加载器一律直接拒绝更早的映像：所有 v1.x 模块与合并前的 2.0
模块都必须重新编译。
v2.0（加载期链接）——布局变更：头部新增模块点分路径
与入口限定名两字段、删除 v1.13 的 `int32 entryPoint`；枚举限定键表与
四张导入槽表随逐单元产物引入。产物不再合并库代码，跨单元符号在
链接期解析。地板自此整体抬到 2.0：加载器按版本检查一律直接拒绝
v1.x 模块（其布局本就会在第一个新字段处解析失败）——旧模块必须
重新编译。
v1.13（限定名键）——布局变更：struct/class/函数表键与流
类型名字面量改为带包名限定（`<包名>.<名字>`；无属主的内建保持裸名），
线上格式在模块名之后新增 `int32 entryPoint` 字段（入口函数索引，模块
不导出入口时为 -1）。旧 ncc 产出的 v1.12 模块会解析错每一个键名，因此
加载器直接拒绝 minor < 13——旧模块必须重新编译。
v1.12（递归类型描述符）——布局变更：每个函数记录
新增形参类型描述符序列，每个 struct/class 字段新增字段类型描述符
（递归类型描述符：嵌套数组、`List`/`Dict` 实例化、struct/class
索引，深度帽 8），记录真实的形参、返回与字段类型。被导入函数桩
以真签名重建（不再以返回 kind 占位），跨模块调用点类型检查因此
与同模块一致；`lib.mk()` 返回 `float[]` 赋给 `int[]` 局部被拒绝、
float 实参加宽与同模块调用完全一致、`out` 实参可往返。旧 ncc
产出的 v1.11 模块缺描述符，因此加载器直接拒绝 minor < 13——旧
模块必须重新编译。
v1.11（泛型数组类型实参）——一次语义下限抬升，不是布
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
（编译所在翻译单元的路径，位于 locals 块之后）；合并进来的函数保留
各自的 `locals`，被导入帧的 GC 根集因此完整。
v1.8——一等函数值：`RTK_Func` kind 字节加上前述 8 条函
数值指令；旧 VM 无法执行这些指令，会直接拒绝 v1.8 模块（下限在此步
升到 minor 8）。
v1.7——stdlib 命名空间内建函数 + 保留命名空间；字段布
局无变化，但关系比较字符串指令共用这一版本步进，旧 VM 必须拒绝这些
模块（下限在此步升到 minor 7）。

每个 struct 包含：name、fieldCount、fieldNames[]、fieldTypeKinds[]、
fieldStructIndices[]、fieldClassIndices[]、fieldTypeDescs[]。

每个 class 包含：name、fieldCount、superClassIdx、fieldNames[]、
fieldTypeKinds[]、fieldStructIndices[]、fieldClassIndices[]、
fieldTypeDescs[]、fieldAccess[]、methodIndices[]、constructorIdx。

## 包归档（.npkg，格式 1.0）

多单元程序与库的分发形态是包归档：头部（包名、格式版本、标志、
预留的签名块描述符，以及**程序包的入口记录**——入口成员的模块路径
加函数名）、成员表（模块路径 → 偏移/长度/校验和，按路径排序保证
字节确定性）与内嵌的 `.ncu` 单元映像。每个成员带一份 FNV-1a 64
校验和（防意外损坏，不是安全机制）。库包是同一容器、不带入口
记录——标准库 `stdlib.npkg` 就是成员为 `io`/`math`/`fs` 的库包。
加载器要求成员映像头部的模块路径与成员表条目一致：名实不符的
产物被拒绝，而不是被静默接受。
