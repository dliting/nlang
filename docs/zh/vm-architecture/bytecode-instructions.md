# 字节码指令


### 核心

| 操作码          | 操作数                      | 说明                           |
|-----------------|-----------------------------|--------------------------------|
| OP_ConstInt32   | int32                       | 把常量压入 pResult             |
| OP_ConstFloat   | float                       | 把常量压入 pResult             |
| OP_ConstZero    | —                           | 把 0 压入 pResult              |
| OP_ConstString  | uint16 poolIdx              | 压入 string 池索引             |
| OP_VarLocal     | uint16 offset               | 把局部变量读入 pResult         |
| OP_Assign       | uint16 dst                  | 把 pResult 存入局部变量        |
| OP_Return       | —                           | 从函数返回                     |

### 算术

| 操作码      | 操作数            | 说明                     |
|-------------|-------------------|--------------------------|
| OP_Add_i32  | dst, src          | locals[dst] += locals[src] |
| OP_Sub_i32  | dst, src          | locals[dst] -= locals[src] |
| OP_Mul_i32  | dst, src          | locals[dst] *= locals[src] |
| OP_Div_i32  | dst, src          | locals[dst] /= locals[src] |
| OP_Mod_i32  | dst, src          | locals[dst] %= locals[src] |
| OP_Neg_i32  | dst               | locals[dst] = -locals[dst] |
| OP_Add_f32  | dst, src          | float 同上               |
| OP_Sub_f32  | dst, src          |                          |
| OP_Mul_f32  | dst, src          |                          |
| OP_Div_f32  | dst, src          |                          |
| OP_Neg_f32  | dst               |                          |

### 比较与逻辑

| 操作码          | 操作数      | 说明                               |
|-----------------|-------------|------------------------------------|
| OP_Less_i32     | lhs, rhs    | locals[lhs] = (a < b) ? 1 : 0      |
| OP_Equal_i32    | lhs, rhs    | locals[lhs] = (a == b) ? 1 : 0     |
| ...             |             | （全部比较运算同此模式）           |
| OP_LogicalNot   | dst         | locals[dst] = !locals[dst]         |

`&&` 与 `||` 没有专用指令——代码生成把它们降级为短路跳转序列
（`OP_JumpIfNot` 加双重 `OP_LogicalNot` 归一化）；实现 `!` 的是
`OP_LogicalNot`。

### 控制流

| 操作码       | 操作数                | 说明                           |
|--------------|-----------------------|--------------------------------|
| OP_Jump      | int16 target          | 无条件跳转                     |
| OP_JumpIfNot | int16 target, uint16  | 局部变量 == 0 时跳转           |

### struct 与 class

| 操作码            | 操作数                          | 说明                     |
|-------------------|---------------------------------|--------------------------|
| OP_AllocStruct    | dst, structIdx, fieldCount      | 在堆上分配 struct        |
| OP_CopyStruct     | dst, src, structIdx             | 深拷贝 struct            |
| OP_New            | dst, classIdx                   | 在堆上分配 class         |
| OP_LoadField      | dst, obj, fieldOff              | 从堆读取字段             |
| OP_StoreField     | obj, fieldOff, src              | 把字段写入堆             |
| OP_NullCheck      | obj                             | 为 null 时抛错           |

### 函数调用

| 操作码              | 操作数                      | 说明                     |
|---------------------|-----------------------------|--------------------------|
| OP_CallFunc         | funcIdx, callParamBase      | 调用函数                 |
| OP_CallMethodDirect | funcIdx, callParamBase      | 调用非虚方法             |
| OP_CallMethod       | methodNameIdx, callParamBase| 按名虚分派               |
| OP_CallIntrinsic    | intrinsicId, callParamBase  | 按 ID 调用内建函数       |

### 函数值（Phase 13）

| 操作码              | 操作数                               | 说明                            |
|---------------------|--------------------------------------|---------------------------------|
| OP_MakeFunc         | funcIdx (uint16)                     | 静态句柄 {funcIdx, 0, 0}        |
| OP_MakeBoundFunc    | funcIdx (uint16)                     | 从 pResult 读取接收者 → {funcIdx, this, static} |
| OP_MakeVFunc        | nameIdx (uint16, string 池)          | 从 pResult 读取接收者 → {nameIdx, this, virtual} |
| OP_CallDelegate     | calleeLocal, callParamBase           | 经句柄调用                      |
| OP_CallDelegateOut  | calleeLocal, callParamBase, outMask (uint32) | 另有 out 写回           |
| OP_Eq_func          | lhs, rhs                             | 句柄内容相等（带 null 守卫）    |
| OP_Ne_func          | lhs, rhs                             | 句柄内容不等                    |
| OP_Func_to_str      | 累加器形式                           | "func N" / "method N"；null → "<null>" |

句柄布局与分派语义见[一等函数值](first-class-function-values.md#first-class-function-values-phase-13)。

### 字符串

| 操作码      | 操作数      | 说明                     |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | 拼接字符串               |
| OP_Eq_str   | lhs, rhs    | 字符串相等               |
| OP_Ne_str   | lhs, rhs    | 字符串不等               |
| OP_Less_str | lhs, rhs    | 按字节关系比较（Phase 11 Step 3b；UTF-8 字节序 == 码点序） |
| OP_LessEqual_str | lhs, rhs | 按字节 `<=`              |
| OP_Greater_str | lhs, rhs | 按字节 `>`               |
| OP_GreaterEqual_str | lhs, rhs | 按字节 `>=`          |
| OP_StrLen   | dst, src    | 字符串长度（字节数，Phase 11 决策 #7） |

### 装箱 / 拆箱 / 向下转型（Phase 8e-1 + 8e-1.5）

| 操作码       | 操作数               | 说明                                          |
|--------------|------------------------|----------------------------------------------|
| OP_Box       | typeKind (uint8)       | 把基本类型值（在 pResult 中）装箱为 Object 引用；分配 RTK_Boxed 堆槽位。值为 0 时短路（保持 null）。 |
| OP_Unbox     | typeKind (uint8)       | 从 pResult（堆索引）中拆出装箱的基本类型值。校验 RTK_Boxed 标签是否匹配，不匹配抛错。 |
| OP_CheckCast | classIdx (uint16)      | 校验 pResult（堆索引）是 classIdx 或其子类（沿运行期父类链上溯）。不匹配抛错。引用原样推回。 |

OP_Box/OP_Unbox 使用 `pResult` 寄存器约定——从 pResult 读入，输出写
回 pResult。隐式转换的发射路径（FixupExprType）把基本类型值包进带 TCK_Box
的 SnCastExpr；显式的 `expr as T` 运算符（Phase 8e-1.5）创建
SnAsExpr，其代码生成按解析出的转换 kind 发射 OP_Unbox/OP_CheckCast。

### 类型转换

| 操作码           | 说明                     |
|------------------|--------------------------|
| OP_CastIntToFloat | int32 → float           |
| OP_CastFloatToInt | float → int32           |
| OP_Int32_to_str  | int32 → string（Phase 8e-9a，经 `std::to_string` 十进制格式化） |
| OP_Float_to_str  | float → string（Phase 8e-9a，`%g` 格式） |
| OP_Enum_to_str   | int32 枚举值 → string（Phase 8e-9b，按名查找） |

字符串强转指令沿用与 int/float 转换相同的 pResult 约定：从 `pResult`
读取源值，把格式化后的字符串压入 `m_stringPool`，再把新字符串索引
（int32）写回 `pResult`。发射模式固定为 `OP_<type>_to_str` 后接
`OP_Assign dst`。

`OP_Enum_to_str` 带一个 uint16 `enumDefIdx` 立即操作数。它从
`pResult` 读取 int32 枚举值，查 `m_compiledModule.enumNames[enumDefIdx][value]`，
把名字字符串压入 `m_stringPool`，再写回索引。值越界时抛错。

发射位点：
- `VmBackend.cpp` 的 `EmitExpression(SnCastExpr&)` 按 `(srcKind,
  dstKind)` 分派 int/float→string（隐式强转）。
- `VmBackend.cpp` 的 `EmitExpression(SnMemberExpr&)` 按
  `outer->EvalDataType()` 为 `.toString()` 调用分派（enum→
  OP_Enum_to_str，int→OP_Int32_to_str，float→OP_Float_to_str，
  string→恒等空操作）。
- `VmBackend.cpp` 的 `EmitExpression(SnCastExpr&)` 处理二元 `+` 的
  强转（另一操作数为 string 时，enum/int/float→string）。

### switch（Phase 12）

| 操作码     | 操作数         | 说明                           |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | 标记（无运行期效果）          |
| OP_Case    | uint16 nextOff  | 标记（由编译器回填）          |

switch 编译为逐标签比较与条件跳转组成的链——没有跳转表指令。比较指
令按判别式的**家族**（而非静态类型）选择：

- int 家族（int 与 enum，按其 int 值比较）→ `OP_Equal`（i32）
- float 家族 → `OP_Equal_f32`（IEEE `==`：`-0.0 == 0.0` 为真，NaN
  永不匹配）
- string 家族 → `OP_Eq_str`（按字节内容比较，池顺序无关）

多值子句（`case 1, 2:`）为每个标签发射一次比较：每个标签的测试命中
时跳到子句体，未命中跳到下一个标签的测试；单标签子句退化为现在的双
跳转形状。一个子句携带 2+N 个跳转、四种目标类别（子句出口 / 下一标
签 / 子句体 / 隐式出口），因此跳转回填器（clause-exit fixup，子句出
口修复）遍历的是变长记录列表——历史上 `i * 2` 的定步长假设已不复存
在。每个 case 体都以一条跳出 switch 的隐式跳转收尾（Java/C# 式禁止
穿透）；显式 `break` 还会弹出 handler，因为它可能从词法上离开 catch
区域。

### 枚举方法调用约定（Phase 12）

枚举方法复用类方法调用路径，只有一条约定：**`this` 是枚举的 int32
值，不是堆引用**。

- 调用在接收者表达式求值进求值认领区（claim area）的槽 0 之后（接收者
  优先的形状，与 `s.equals` 相同）发射 `OP_CallMethodDirect funcIdx callParamBase`。
- 被调帧的 `this` 局部变量以 typeKind `RTK_Int32` 分配（类方法用
  `RTK_Class`）。这一点很关键：GC 根扫描按 typeKind 遍历局部变量——
  枚举的 `this` 若按 class 建档，就会被当作堆索引追踪并破坏堆。
- 表示决策（D3）：枚举在运行期保持 nominal int32——`==`、
  `switch`、实参传递与 `.nmod` 序列化全部不受影响。Java 式的堆单例
  枚举需要模块级实例初始化子系统（init 函数执行顺序 + GC 根），
  被有意推迟。

### 其他

| 操作码       | 操作数         | 说明                     |
|--------------|----------------|--------------------------|
| OP_ParaEnd   | —              | 参数列表结束             |
| OP_DebugInfo | uint16         | 调试行信息               |
