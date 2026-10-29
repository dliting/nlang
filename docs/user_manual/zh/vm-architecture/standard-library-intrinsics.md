# 标准库内建函数

内建函数（intrinsic）是VM直接以C++实现的操作：它们没有NLang函数体。只有字符串方法家族经`OP_CallIntrinsic`调用、不占模块的函数表；其余家族——字节流与文件流、`List<T>`/`Dict<K,V>`、Object协议、异常构造函数与并发操作——都搭载一条盖有内建函数ID的桩`CompiledFunction`记录，经普通的方法或函数调用指令分派。标准库横跨这几套机制：`math`、`io`、`fs`是普通的库源码，其native成员由`nlang_<包名>.dll`提供；`thread`的native声明由虚拟机本体以内建桩实现；`concurrent`是纯NLang实现——设计见[库机制](library-mechanism.md)。形如`math.sin(x)`的调用编译为`OP_CallFunc`，指向内联进来的库声明，因此和用户函数一样带有一条`CompiledFunction`记录。

**哪些调用走到`OP_CallIntrinsic`**：字符串方法家族（`s.substring(1)`、`s.split(d)`……）。签名由预置面类`String`（`stdlib/builtins.n`）声明——resolver按该声明校验成员调用，发射时由`src/vm/backend/EmitExprMemberString.cpp`从绑定表（`src/vm/backend/BindingTable.cpp`，键`String.<方法名>`）读取内建函数标识符（ID，identifier）。内建函数ID是模块局部的——链接器的操作数重映射（`NcuRemapBytecodeOperands`，`src/vm/NcuLinkerRemap.cpp`）从不改写它——因此跨模块导入不存在ID问题。

**实参应用二进制接口（ABI，application binary interface）**：接收者占据`callParamBase[0]`，实参从槽1起，即`string.equals`的形状。`OP_CallIntrinsic`不带实参个数，因此唯一省略尾部实参的调用形态——`substring(start)`——会合成`end`，取接收者的长度。

**VM分派链**：`ExecuteIntrinsic`（VmExecutorIntrinsics.cpp）委托给每个家族编译单元（TU，translation unit）的一个成员函数；ID不归其管时返回`false`，链条继续穿透——ByteStream → FileStream → 线程家族分支（`IntrinsicsThread.cpp`，ID 129-143）→ 内联的Object/List/Dict协议与异常构造函数分支 → string → 未知ID抛错。每个家族在自己的TU里独立扩展。

**内建函数ID分配**（`include/nlang/vm/Intrinsics.h`）。各块连续。字符串方法块两侧静态绑定——块边界在`Intrinsics.h`，绑定表每个方法一行、行数在编译期对照块大小检查——不一致是编译错误，不是运行期的「未知内建函数」漏洞；其余家族没有可绑定的源码表：

| ID范围 | 前缀 | 家族 |
|-----|--------|--------|
| 0-18 | INTR_BS_* | ByteStream |
| 20-37 | INTR_FS_* | FileStream（文件流，与`fs`库无关） |
| 40-43, 61-63 | INTR_Object_/String_/List_/Dict_ | 协议方法 + toString |
| 44-52 | INTR_List_* | List\<T\>方法 |
| 53-60 | INTR_Dict_* | Dict\<K,V\>方法 |
| 64-69 | INTR_*Exception_Ctor | 异常构造函数，含IOException |
| 95-112 | INTR_String_* | 字符串方法，18个（Equals/GetHashCode在42/43） |
| 128, 144 | INTR_InvalidCastException_Ctor / INTR_ChannelClosedException_Ctor | 字符串块之后追加的异常构造函数 |
| 129-133 | INTR_Thread_* | `thread`命名空间自由函数 |
| 134-141 | INTR_Mutex_/INTR_Channel_ | Mutex/Channel方法 |
| 142-143 | INTR_Future_* | Future方法 |

分配只追加：空位19与38-39保留；70-94与113-127两段已退役，不再发放。

**关系比较指令**：`OP_Less_str` / `OP_LessEqual_str` /
`OP_Greater_str` / `OP_GreaterEqual_str`——按字节的关系比较（Unicode转换格式（UTF-8，Unicode Transformation Format）字节序 == 码点序），与`OP_Eq_str`呼应。变体分派以左操作数的`EvalDataType`为键，与所有二元指令一致。

**位置数组守卫**：`s_OpCodeNames`（OpCodeTable.cpp）是位置数组——缺一行不是编译错误，而是运行期的越界读。`static_assert(std::size(s_OpCodeNames) == static_cast<size_t>(OpCode::OP_Count))`在编译期锁定尺寸。新增一条指令共6处手工触点：枚举、名字表、重映射步长`NcuInstructionStride`（NcuLinkerRemap.cpp；默认`assert(false)`——Release下漏写会破坏跨模块重映射）、代码生成、执行器、ndisasm（自己的`InstructionStride`游走与打印分支）。
