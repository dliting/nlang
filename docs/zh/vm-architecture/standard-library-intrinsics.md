# 标准库内建函数

标准库函数（`math.sin(x)`、`io.print(s)`、`fs.join(a,b)`）与内建字
符串方法由 VM 直接以 C++ 实现，称为内建函数（intrinsic）：它们没有
NLang 函数体，也不占模块的函数表。本页说明这类调用如何编译、id 如
何分配，以及新增指令时要触及的位置。

命名空间限定的调用（`math.sin(x)`、`io.print(s)`、`fs.join(a,b)`）
与内建字符串方法编译为 `OP_CallIntrinsic`——零 `CompiledFunction`
记录、零 `.nmod` 函数条目。编译器侧的事实来源是
`include/nlang/vm/StdLib.h` 中的单一表对（`kStdLibTable` +
`kStringMethodTable`）：resolver 拦截限定调用与成员调用并对照该表，
`VmBackend::EmitStdlibCall` 发射调用。内建函数 id 是模块局部的——
`RemapBytecode` 从不改动它们——因此跨模块导入不存在 id 问题。

**实参 ABI**——两种形状，有意区分：

- 命名空间自由函数从 `callParamBase` 槽 0 起读取实参，**没有
  `this` 指针**（不同于其他所有内建函数家族——它们的 `this` 占据
  槽 0）；
- 字符串方法沿用 `string.equals` 的 ABI：接收者字符串句柄在
  `callParamBase[0]`，实参从槽 1 起。

**VM 分派链**：`ExecuteIntrinsic`（VmExecutor.cpp）委托给每个家族
TU 的一个成员函数；id 不归其管时返回 `false`，链条继续穿透——math
→ io → string → fs → 未知 id 抛错。每个家族在自己的 TU 里独立扩
展。

**内建函数 id 分配**（CompiledModule.h）。各块连续，且两侧都与
StdLib.h 的表静态绑定（每个条目的 id 都落在自己块内，条目数 == 块
大小——不一致是编译错误，不是运行期的「未知内建函数」漏洞）：

| Id 范围 | 前缀 | 家族 |
|-----|--------|--------|
| 0-14 | INTR_BS_* | ByteStream |
| 20-33 | INTR_FS_* | FileStream（下表 filesystem 块用 `FileSystem_` 的原因） |
| 40-43, 61-63 | INTR_Object_/String_/List_/Dict_ | 协议方法 + toString |
| 44-52 | INTR_List_* | List\<T\> 方法 |
| 53-60 | INTR_Dict_* | Dict\<K,V\> 方法 |
| 64-69 | INTR_*Exception_Ctor | 异常构造函数，含 IOException |
| 70-94 | INTR_Math_* | math，25 个函数 |
| 95-106 | INTR_String_* | 字符串方法，12 个（Equals/GetHashCode 在 42/43） |
| 110-114 | INTR_Io_* | io，5 个函数 |
| 120-127 | INTR_FileSystem_* | fs，8 个函数 |

**关系比较指令**：`OP_Less_str` / `OP_LessEqual_str` /
`OP_Greater_str` / `OP_GreaterEqual_str`——按字节的关系比较（UTF-8
字节序 == 码点序），与 `OP_Eq_str` 呼应。变体分派以左操作数的
`EvalDataType` 为键，与所有二元指令一致。

**位置数组守卫**：`s_OpCodeNames`（OpCodeTable.cpp）是位置数组——
缺一行不是编译错误，而是运行期的越界读。
`static_assert(std::size(s_OpCodeNames) == +OpCode::OP_Count)`
在编译期锁定尺寸。新增一条指令共 6 处手工触点：枚举、名字表、
`InstructionStride`（默认 `assert(false)`——Release 下漏写会破坏跨
模块重映射）、代码生成、执行器、ndisasm。
