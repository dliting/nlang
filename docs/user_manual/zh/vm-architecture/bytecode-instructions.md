# 字节码指令

本页是NLang字节码指令集的参考表：编译器（`VmBackend`）把抽象语法树（AST，abstract syntax tree）翻译成这些指令，执行器（`VmExecutor`）逐条解释。多数指令围绕结果寄存器`pResult`与局部变量槽位读写。`OP_*`名称是源码中的真实标识符，可作为检索起点。

数值指令不在名字里编码类型：单一指令码用`u8 kind`立即数携带操作数类型（标量RTK编码，经登记表实例化的运算表分派）。编译器如何选定kind见[二元表达式提升](binary-expression-promotion.md)。

### 核心

| 操作码          | 操作数                      | 说明                           |
|-----------------|-----------------------------|--------------------------------|
| OP_ConstInt32   | int32                       | 把常量压入pResult             |
| OP_ConstFloat   | float                       | 把常量压入pResult             |
| OP_ConstInt64   | int64                       | 把8字节常量压入pResult       |
| OP_ConstDouble  | double                      | 把8字节常量压入pResult       |
| OP_ConstZero    | —                           | 把0压入pResult              |
| OP_ConstString  | uint16 constIdx             | 压入字符串常量句柄             |
| OP_VarLocal     | uint16 offset               | 把局部变量读入pResult         |
| OP_Assign       | uint16 dst                  | 把pResult存入局部变量        |
| OP_Return       | —                           | 从函数返回                     |
| OP_Stop         | —                           | 停止执行                       |
| OP_AssertFail   | uint16 msgIdx               | 抛出"assertion failed: <msg>" |

### 算术

| 操作码      | 操作数                | 说明                     |
|-------------|-----------------------|--------------------------|
| OP_Add      | kind, dst, src        | locals[dst] += locals[src] |
| OP_Sub      | kind, dst, src        | locals[dst] -= locals[src] |
| OP_Mul      | kind, dst, src        | locals[dst] *= locals[src] |
| OP_Div      | kind, dst, src        | locals[dst] /= locals[src] |
| OP_Mod      | kind, dst, src        | locals[dst] %= locals[src]；float行算fmod |
| OP_Neg      | kind, dst             | locals[dst] = -locals[dst] |
| OP_Shl      | kind, dst, src        | locals[dst] <<= 计数；计数必须在[0, 位宽)内，否则VM抛具名错误 |
| OP_Shr      | kind, dst, src        | 同上；有符号行算术右移，无符号行逻辑右移 |
| OP_BitAnd   | kind, dst, src        | locals[dst] &= locals[src] |
| OP_BitOr    | kind, dst, src        | locals[dst] |= locals[src] |
| OP_BitXor   | kind, dst, src        | locals[dst] ^= locals[src] |
| OP_BitNot   | kind, dst             | locals[dst] = ~locals[dst] |

`kind`是标量RTK立即数，选定登记表实例化运算表的行（int32、float、byte、ulong等）。

### 比较与逻辑

| 操作码          | 操作数                | 说明                               |
|-----------------|-----------------------|------------------------------------|
| OP_Cmp          | kind, cmpOp, lhs, rhs | 向locals[lhs]写int32的0/1；cmpOp ∈ {0 Less, 1 LessEqual, 2 Greater, 3 GreaterEqual, 4 Equal, 5 NotEqual} |
| OP_LogicalNot   | dst                   | locals[dst] = !locals[dst]         |

`&&`与`||`没有专用指令——代码生成把它们降级为短路跳转序列（`OP_JumpIfNot`加双重`OP_LogicalNot`归一化）；实现`!`的是`OP_LogicalNot`。字符串与函数值的比较走各自的专用指令（见下文「字符串」与「函数值」）。

### 控制流

| 操作码       | 操作数                | 说明                           |
|--------------|-----------------------|--------------------------------|
| OP_Jump      | uint16 target         | 无条件跳转                     |
| OP_JumpIfNot | uint16 target, uint16 localOff | int32局部变量 == 0时跳转 |

### struct与class

| 操作码            | 操作数                          | 说明                     |
|-------------------|---------------------------------|--------------------------|
| OP_AllocStruct    | dst, structIdx, fieldCount      | 在堆上分配struct        |
| OP_CopyStruct     | dst, src, structIdx             | 深拷贝struct（建立新记录） |
| OP_CopyStructInPlace | dst, src, structIdx          | 把src记录的值原位写入dst已有的记录；dst≤0时建立一条新记录并返回其句柄，dst==src为空操作 |
| OP_New            | dst, classIdx, descIdx          | 在堆上分配class；descIdx为泛型class指定实例化描述符（0xFFFF = 无） |
| OP_LoadField      | dst, obj, fieldOff              | 从堆读取字段             |
| OP_StoreField     | obj, fieldOff, src              | 把字段写入堆             |
| OP_NullCheck      | obj                             | 为null时抛错           |

### 数组

| 操作码               | 操作数                               | 说明                     |
|----------------------|--------------------------------------|--------------------------|
| OP_AllocArrayN       | dst, arrayTypeIdx, rank, dimsBase, descIdx | 分配数组；从dimsBase起的rank个连续槽读取各维元素个数；descIdx为泛型元素行键定元素描述符检查（0xFFFF = 无） |
| OP_AssembleArrayN    | dst, arrayTypeIdx, elemsBase, count, descIdx | 组装数组字面量：把elemsBase起的count个连续槽元素拷入新记录（descIdx同上） |
| OP_LoadElementN      | dst, arr, idxBase, rank              | 按idxBase起的rank个坐标读取元素 |
| OP_StoreElementN     | arr, idxBase, src, rank              | 按idxBase起的rank个坐标写入元素 |
| OP_LoadElementFlat   | dst, arr, flatIdx                    | 按行主序扁平下标读取元素（foreach遍历用） |
| OP_ArrayLength       | dst, arr                             | 全部元素个数 → int32    |
| OP_ArrayShape        | dst, arr                             | 创建逐维元素个数的一维int数组 |

一维数组是`rank=1`的特例，与多维数组共用同一组指令；各维元素个数在初始化器降级时是编译期常量，运行期创建（尺寸来自表达式）则从求值暂存槽读取。

### 函数调用

| 操作码                 | 操作数                      | 说明                     |
|------------------------|-----------------------------|--------------------------|
| OP_CallStatic          | funcIdx, callParamBase, descIdx | 无接收者的直接调用（自由函数、静态成员）；descIdx为泛型被调帧填充帧描述符槽 |
| OP_CallMethodDirect    | funcIdx, callParamBase      | 调用非虚方法             |
| OP_CallMethod          | methodNameIdx, callParamBase| 按方法名虚分派               |
| OP_CallIntrinsic       | intrinsicId, callParamBase  | 按ID调用内建函数       |
| OP_ParaEnd             | —                           | 参数列表结束             |

静态调用族与`OP_MakeFunc`上的uint16 `descIdx`操作数指向模块的泛型实例化描述符表（`.ncu`模板表，规格§28.2.1）中的一行：对泛型函数或泛型类成员的调用携带该描述符，被调帧的`T`读取与kind分派都以其真类型实参为键。`0xFFFF`标记非泛型调用点；`0xFFFE`继承调用方自己的描述符槽（泛型体转发到另一个泛型）。

### 函数值

| 操作码              | 操作数                               | 说明                            |
|---------------------|--------------------------------------|---------------------------------|
| OP_MakeFunc         | funcIdx, descIdx (各uint16)          | 静态句柄 {funcIdx, 0, 0, descIdx}；descIdx为泛型目标配对的实例化描述符行 |
| OP_MakeBoundFunc    | funcIdx (uint16)                     | 从pResult读取接收者 → {funcIdx, this, static} |
| OP_MakeVFunc        | nameIdx (uint16, 字符串常量表)       | 从pResult读取接收者 → {nameIdx, this, virtual} |
| OP_CallDelegate     | calleeLocal, callParamBase           | 经句柄调用                      |
| OP_Eq_func          | lhs, rhs                             | 句柄内容相等（带null守卫）    |
| OP_Ne_func          | lhs, rhs                             | 句柄内容不等                    |
| OP_Eq_struct        | lhs, rhs                             | 结构体按字段值相等              |
| OP_Ne_struct        | lhs, rhs                             | 结构体按字段值不等              |
| OP_Func_to_str      | pResult累加器                        | "func <名>" / "method <名>"；null → "<null>" |

句柄布局与分派语义见[一等函数值](first-class-function-values.md#一等函数值)。

### 泛型类型参数

泛型体在运行期经调用点传入的实例化描述符读取真类型实参（上文调用与分配族的`descIdx`操作数）。下列指令以接收者的描述符为键；`paramOrdinal`索引声明列表中的类型形参。

| 操作码              | 操作数                      | 说明                     |
|---------------------|-----------------------------|--------------------------|
| OP_TypeNameArg      | dst, paramOrdinal (uint8)   | 读取接收者的实例化描述符，取第paramOrdinal个类型实参的规范拼写，创建字符串（`typeName(T)`） |
| OP_TypeNameCompose  | dst, compositionIdx         | 按拼接记录对接收者的实例化组合嵌套拼写并驻留（`typeName(List<T>)`） |
| OP_Eq_T             | dst, paramOrdinal, lhs, rhs | 按第paramOrdinal个类型实参的kind比较两个T型槽位（标量单元、字符串内容、struct字段值、class/数组同一性、函数记录内容）；lhs/rhs的`0xFFFF`标记null字面量一侧 |
| OP_Ne_T             | dst, paramOrdinal, lhs, rhs | OP_Eq_T的取反判定 |
| OP_TypeArg_to_str   | paramOrdinal                | 经该kind自身的格式化渲染pResult中的T型值 |
| OP_As_TypeParam     | paramOrdinal                | `x as T`：按接收者的类型实参kind裁决转换；不匹配抛可捕获的InvalidCastException，null原样通过 |
| OP_Box_TypeArg      | paramOrdinal                | 把T型累加器归一为擦除容器的元素形态（标量kind按描述符的标签装箱，引用kind原样通过） |
| OP_Unbox_TypeArg    | paramOrdinal                | 逆边界：擦除容器元素归一为T型帧形态（标量kind按描述符的标签拆箱，引用kind原样通过） |

Box/Unbox对维护统一T表示不变量：泛型帧中的T型值当且仅当T的kind为标量时是原始单元，而擦除容器（`List<T>`、`Dict<K,V>`、通道）把每个元素都存为堆句柄。二者发射在静态装箱方案无法解析标签的位置——接收者的类型实参是调用方泛型体自己的占位符，由执行帧的描述符裁决。

描述符线格式见[`.ncu`单元映像](../specification/formats/28-ncu.md)；编译器如何生成并绑定逐实例化的函数体是[库机制](library-mechanism.md)的主题。

### 字符串

| 操作码      | 操作数      | 说明                     |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | 拼接字符串               |
| OP_Eq_str   | lhs, rhs    | 字符串相等               |
| OP_Ne_str   | lhs, rhs    | 字符串不等               |
| OP_Less_str | lhs, rhs    | 按字节关系比较（Unicode转换格式（UTF-8，Unicode Transformation Format）字节顺序 == 码点序）；null操作数按空串参与比较 |
| OP_LessEqual_str | lhs, rhs | 按字节`<=`              |
| OP_Greater_str | lhs, rhs | 按字节`>`               |
| OP_GreaterEqual_str | lhs, rhs | 按字节`>=`          |
| OP_StrLen   | dst, src    | 字符串长度（字节数） |
| OP_StrByteAt | dst, str, idx | 把字符串第idx个字节作为ubyte读入dst；越界抛IndexOutOfBoundsException |
| OP_StrForeachStep | str, off, cond, ch | 一次码点迭代步进：把字节偏移`off`处的码点解码进`ch`，`off`按其编码长度前进，向`cond`写1/0继续标志 |

### 装箱 / 拆箱 / 向下转型

| 操作码       | 操作数               | 说明                                          |
|--------------|------------------------|----------------------------------------------|
| OP_Box       | typeKind (uint8)       | 把基本类型值（在pResult中）装箱为Object引用；一律分配RTK_Boxed堆槽位。null字面量从不装箱——编译器跳过包装，null保持0哨兵。 |
| OP_Unbox     | typeKind (uint8)       | 从pResult（堆索引）中拆出装箱的基本类型值。校验RTK_Boxed标签是否匹配；null/无效引用、非装箱目标或标签错配都抛错。 |
| OP_CheckCast | classIdx (uint16), descIdx (uint16) | 校验pResult（堆索引）是classIdx或其子类（沿运行期父类链上溯）；descIdx指向实例化描述符行时，还比对实例化拼写。不匹配抛错。引用原样推回。 |

OP_Box/OP_Unbox使用`pResult`寄存器约定——从pResult读入，输出写回pResult。隐式转换的发射路径（FixupExprType）把基本类型值包进带TCK_Box的SnCastExpr；显式的`expr as T`运算符创建SnAsExpr，其代码生成按解析出的转换kind发射OP_Unbox/OP_CheckCast。

### 类型转换

| 操作码           | 操作数                  | 说明                     |
|------------------|-------------------------|--------------------------|
| OP_PrimCast      | srcKind, dstKind        | 在pResult上就地转换resolver放行的任意两个标量行（int↔float、加宽、常量适配的收窄） |
| OP_Prim_to_str   | kind                    | 按该行的ValueToString把pResult中的标量格式化，创建字符串对象并写回句柄（bool渲染true/false，char编码其码点） |
| OP_Enum_to_str   | enumDefIdx (uint16)     | int32枚举值 → string（按名称查找） |
| OP_Array_to_str  | —                       | 把pResult中的数组格式化为"[e1, e2, ...]" |

字符串强转指令沿用与数值转换相同的pResult约定：从`pResult`读取源值，把格式化后的字符串创建为字符串对象，再把新句柄（int32）写回`pResult`；发射路径随后用`OP_Assign dst`落存。

`OP_Enum_to_str`带一个uint16 `enumDefIdx`立即操作数。它从`pResult`读取int32枚举值，查`m_compiledModule.enumNames[enumDefIdx][value]`，把名字创建为字符串对象，再写回句柄。值越界时抛错。

发射位置：
- `EmitExprCast.cpp`的`Access(SnCastExpr&)`对resolver包装的每个隐式标量转换发射`OP_PrimCast`/`OP_Prim_to_str`——包括另一操作数为string时的二元`+`强转（enum在塌缩为int之前先走`OP_Enum_to_str`）。
- `EmitExprMemberString.cpp`按接收者类型为`.toString()`调用分派：enum→`OP_Enum_to_str`、任意标量→`OP_Prim_to_str`、数组→`OP_Array_to_str`、string→恒等空操作。

### 异常处理

| 操作码        | 操作数       | 说明                                         |
|---------------|--------------|----------------------------------------------|
| OP_Throw      | uint16 src   | 抛出locals[src]处的Exception对象            |
| OP_Rethrow    | —            | 重抛当前catch的异常                          |
| OP_PopHandler | —            | catch退出时弹出一个handler条目             |

### switch

| 操作码     | 操作数         | 说明                           |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | 标记（无运行期效果）          |
| OP_Case    | uint16 nextOff  | 标记（由编译器回填）          |

switch编译为逐标签比较与条件跳转组成的链——没有跳转表指令。比较指令按判别式的**家族**（而非静态类型）选择：

- int家族（int与enum，按其int值比较）→ `OP_Cmp` Equal
- float家族 → float行的`OP_Cmp` Equal（IEEE `==`：`-0.0 == 0.0`为真，NaN永不匹配）
- string家族 → `OP_Eq_str`（按字节内容比较，常量表顺序无关）

多值子句（`case 1, 2:`）为每个标签发射一次比较：每个标签的测试命中时跳到子句体，未命中跳到下一个标签的测试；单标签子句退化为双跳转形状。一个子句携带2+N个跳转、四种目标类别（子句出口 / 下一标签 / 子句体 / 隐式出口），因此跳转回填器（clause-exit fixup，子句出口修复）遍历的是变长记录列表，而非按定步长定位记录。每个case体都以一条跳出switch的隐式跳转收尾（Java/C#式禁止穿透）；显式`break`还会弹出handler，因为它可能从词法上离开catch区域。

### 枚举方法调用约定

枚举方法复用类方法调用路径，只有一条约定：**`this`是枚举的int32值，不是堆引用**。

- 调用在接收者表达式求值进求值暂存区的槽0之后（接收者优先的形状，与`s.equals`相同）发射`OP_CallMethodDirect funcIdx callParamBase`。
- 被调帧的`this`局部变量以typeKind `RTK_Int32`分配（类方法用`RTK_Class`）。这一点很关键：垃圾回收（GC，garbage collection）根扫描按typeKind遍历局部变量——枚举的`this`若按class建档，就会被当作堆索引追踪并破坏堆。
- 表示决策：枚举在运行期保持nominal int32——`==`、`switch`、实参传递与`.ncu`序列化全部不受影响。Java式的堆单例枚举需要模块级实例初始化子系统（init函数执行顺序 + GC根），当前不支持。

### 其他

| 操作码       | 操作数         | 说明                     |
|--------------|----------------|--------------------------|
| OP_DebugInfo | uint16 line    | 调试行信息（兼调试器检查点） |
