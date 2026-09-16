# 二元表达式提升（Phase 8e-8）


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

**FixupExprType 修复**：`FixupExprType` 现在会给新建的 `SnCastExpr`
设置 `EvalDataType`（Phase 8e-8）。此前只有显式的 `expr as T` 路径
会设置它；经 FixupExprType 创建的隐式转换（赋值 / 实参 / 二元提升）
不设 EvalDataType，下游消费者看到的便是 null 类型。

没有新指令。复用前几阶段的 `OP_CastIntToFloat` / `OP_CastFloatToInt`
/ `OP_Add_f32` 等。
