# 标准库内建函数

内建函数（intrinsic）是VM直接以C++ 实现的操作：它们没有NLang
函数体，也不占模块的函数表。`OP_CallIntrinsic`承载的是按接收者分
派的内建能力——字节流与文件流、`List<T>`/`Dict<K,V>`、Object协议、
异常构造函数与字符串方法。标准库不属于这套机制：`math`、`io`、`fs`
是普通的库源码，其native成员由`nlang_<ns>.dll`提供，设计见
[库机制](library-mechanism.md)。形如`math.sin(x)`的调用编译为
`OP_CallFunc`，指向内联进来的库声明，因此和用户函数一样带有一条
`CompiledFunction`记录。

**哪些调用走到`OP_CallIntrinsic`**：字符串方法家族（`s.substring(1)`、
`s.split(d)`……），通过`include/nlang/vm/StdLib.h`的
`kStringMethodTable`解析——resolver用该表拦截成员调用，
`src/vm/backend/EmitExprMemberString.cpp`发射调用。内建函数id是
模块局部的——`RemapBytecode`从不改动它们——因此跨模块导入不存在
id问题。

**实参应用二进制接口（ABI，application binary interface）**：接收者占据`callParamBase[0]`，实参从槽1起，即
`string.equals`的形状。`OP_CallIntrinsic`不带实参个数，因此短于
表项允许长度的调用会合成缺失的尾部实参（`StringTrailingDefault`）。

**VM分派链**：`ExecuteIntrinsic`（VmExecutorIntrinsics.cpp）委托给
每个家族编译单元（TU，translation unit）的一个成员函数；id不归其管时返回`false`，链条继续穿
透——ByteStream → FileStream → 内联的Object/List/Dict协议臂 →
string → 未知id抛错。每个家族在自己的TU里独立扩展。

**内建函数id分配**（CompiledModule.h）。各块连续，且两侧都与
StdLib.h的表静态绑定（每个条目的id都落在自己块内，条目数 == 块
大小——不一致是编译错误，不是运行期的「未知内建函数」漏洞）：

| Id范围 | 前缀 | 家族 |
|-----|--------|--------|
| 0-14 | INTR_BS_* | ByteStream |
| 20-33 | INTR_FS_* | FileStream（文件流，与`fs`库无关） |
| 40-43, 61-63 | INTR_Object_/String_/List_/Dict_ | 协议方法 + toString |
| 44-52 | INTR_List_* | List\<T\> 方法 |
| 53-60 | INTR_Dict_* | Dict\<K,V\> 方法 |
| 64-69 | INTR_*Exception_Ctor | 异常构造函数，含IOException |
| 95-106 | INTR_String_* | 字符串方法，12个（Equals/GetHashCode在42/43） |

分配只追加：映射中的空档（15-19、34-39、70-94以及107之后的全
部id）不会再被发放。

**关系比较指令**：`OP_Less_str` / `OP_LessEqual_str` /
`OP_Greater_str` / `OP_GreaterEqual_str`——按字节的关系比较（Unicode转换格式（UTF-8，Unicode Transformation Format）
字节序 == 码点序），与`OP_Eq_str`呼应。变体分派以左操作数的
`EvalDataType`为键，与所有二元指令一致。

**位置数组守卫**：`s_OpCodeNames`（OpCodeTable.cpp）是位置数组——
缺一行不是编译错误，而是运行期的越界读。
`static_assert(std::size(s_OpCodeNames) == +OpCode::OP_Count)`
在编译期锁定尺寸。新增一条指令共6处手工触点：枚举、名字表、
`InstructionStride`（默认`assert(false)`——Release下漏写会破坏跨
模块重映射）、代码生成、执行器、ndisasm。
