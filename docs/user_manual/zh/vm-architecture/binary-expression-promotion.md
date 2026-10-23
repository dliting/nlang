# 二元表达式提升

VM的算术指令用单一指令码加`u8 kind`立即数编码操作数类型（`OP_Add`、`OP_Sub`、`OP_Cmp`、`OP_Shl`等），而语言允许混写各种数值类型。二元表达式提升就是补齐这一差距的机制：编译期把两个操作数统一到同一个公共类型，指令再把该类型作为kind立即数发射。本页说明提升规则在resolver与代码生成两处的实现。

## 提升规则

算术二元表达式（`+ - * / %`）采用**对称数值提升**：两个操作数先提升到公共类型再执行指令。公共类型由标量类型登记表算出（`CommonNumericType`）：比`int`窄的整型先升到`int`（C#规则——`byte + byte`结果是`int`）；其余整型行取能隐式接收两者的最小类型（`int + uint`是`long`，`uint + ulong`是`ulong`）；float操作数压过任何整型，且较宽的float档位获胜（`float + double`是`double`）。没有隐式公共类型的一对是编译错误（`no implicit common type for "Int32" and "ULong".`）。`string`参与`+`时可与任意操作数拼接（非string一侧被包装并在运行期格式化），string上的其他运算是编译错误，`null`不是合法算术操作数（`null is not a valid arithmetic operand.`）。

位移（`<<` `>>`）是非对称的例外：左操作数的类型行决定结果类型（比`int`窄的左操作数升到`int`），计数须能隐式转换到该类型——更宽或异符号的计数被拒绝（`operator '<<' requires a count implicitly convertible to "Int32", got "UInt32".`），能放进该类型的常量则获准。计数越界由VM在运行期检查。

比较（`< <= > >= == !=`）与逻辑（`&& ||`）表达式的结果类型是`bool`（以int32的0/1承载）。比较先过类型门——`string`只能与`string`或`null`比较，函数值与数组只支持`==`/`!=`，`bool`与`char`各有误用门——再把混写的数值操作数按算术同样的规则提升到公共类型（`1 < 2.5`按`double`比较）；同类型对、class/enum同一性比较与`null`比较不做包装。

## resolver

（`ExprResolverBinary.cpp`的`Access(SnBinaryExpr&)`）：
1. 分派：比较与逻辑运算符先过类型门、结果为NK_Bool；位移走`ResolveShiftBinary`；其余走`ResolveArithmeticBinary`。
2. 按上述规则计算T_result。
3. 设置`sn.EvalDataType(T_result)`。
4. 若操作数类型与T_result不同，用`FixupExprType`把它包进`SnCastExpr`。包装后，`sn.Children()[0]/[1]`持有（可能已转换的）操作数；`sn.Left()/Right()`已过期但不再被使用。

## 代码生成

（`EmitExprBinary.cpp`二元路径）：遍历的是`sn.Children()`而非`Left()/Right()`，并按`leftChild.EvalDataType()`分派指令（enum归一为int32）。因为resolver已把两个操作数都包装过，算术与位移的左子节点kind即T_result，混合比较的即公共类型。`string`走专用指令（`OP_Concat_str`、`OP_Eq_str`、`OP_Less_str`家族——按Unicode码点字节序），函数值走`OP_Eq_func`/`OP_Ne_func`，其余走kind立即数家族（`OP_Add`等；`OP_Cmp`另带比较运算符立即数）。`%`同表分派：整型行算`%`，float行算fmod。`&&`/`||`降级为短路分支（`OP_JumpIfNot`加双`OP_LogicalNot`归一），不占用求值区槽位；二元路径把右操作数停放在逐层`EvalAreaClaim`里。

## FixupExprType与EvalDataType

经`FixupExprType`新建的`SnCastExpr`一律设置`EvalDataType`，显式的`expr as T`路径也是如此。隐式转换（赋值 / 实参 / 二元提升）的包装节点因此都带有确定的类型，下游消费者不会读到null类型。会丢精度的包装会报精度丢失警告，`--no-warn`可屏蔽。

没有新指令。复用既有的kind立即数家族（`OP_Add`、`OP_Cmp`、`OP_Shl`等）与单一转换指令`OP_PrimCast`（源类型与目标类型都是立即数）。
