# 二元表达式提升

VM 的算术指令按具体类型编码（`OP_Add_i32`、`OP_Add_f32` 等变体），
而语言允许混写 `int` 与 `float`。二元表达式提升就是补齐这一差距的
机制：编译期把两个操作数统一到较宽的类型，再发射对应类型的指令。
本页说明提升规则在 resolver 与代码生成两处的实现。

算术二元表达式（`+ - * / %`）采用**对称数值提升**：两个操作数先提
升到较宽的类型再执行指令。提升规则：`int OP int → int`；
`int OP float` / `float OP int → float`；`float OP float → float`；
`string + string → string`（仅拼接——string 上的其他运算是编译错
误）。

**resolver**（`ExprResolver.cpp` 的 `Access(SnBinaryExpr&)` else 分
支）：
1. 按上述规则计算 T_result（string 仅在 OP_Add 时短路为 NK_String；
   算术运算只要有 float 操作数就偏向 NK_Float；否则 NK_Int32）。
2. 设置 `sn.EvalDataType(T_result)`。
3. 若操作数类型与 T_result 不同，用 `FixupExprType` 把它包进
   `SnCastExpr`。包装后，`sn.Children()[0]/[1]` 持有（可能已转换的）
   操作数；`sn.Left()/Right()` 已过期但不再被使用。

**代码生成**（`VmBackend.cpp` 二元路径）：遍历的是 `sn.Children()`
而非 `Left()/Right()`，并按 `leftChild.EvalDataType()` 分派指令。
算术运算中 leftChild 是 EvalDataType 为 T_result 的 SnCastExpr；比
较运算（不包装）中 leftChild 是原始操作数，其类型选择 i32/f32/str
变体（`string == string` 用 `OP_Eq_str`，即使 `bin.EvalDataType()`
对比较而言恒为 NK_Int32）。

**FixupExprType 与 EvalDataType**：经 `FixupExprType` 新建的
`SnCastExpr` 一律设置 `EvalDataType`，显式的 `expr as T` 路径也是如
此。隐式转换（赋值 / 实参 / 二元提升）的包装节点因此都带有确定的类
型，下游消费者不会读到 null 类型。

没有新指令。复用既有的 `OP_CastIntToFloat` / `OP_CastFloatToInt`
/ `OP_Add_f32` 等。
