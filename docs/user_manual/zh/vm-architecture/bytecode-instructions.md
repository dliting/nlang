# 字节码指令

本页是NLang字节码指令集的参考表：编译器（`VmBackend`）把抽象语法树（AST，abstract syntax tree）翻译
成这些指令，执行器（`VmExecutor`）逐条解释。多数指令围绕结果寄存器
`pResult`与局部变量槽位读写。`OP_*`名称是源码中的真实标识符，可作
为检索起点。

### 核心

| 操作码          | 操作数                      | 说明                           |
|-----------------|-----------------------------|--------------------------------|
| OP_ConstInt32   | int32                       | 把常量压入pResult             |
| OP_ConstFloat   | float                       | 把常量压入pResult             |
| OP_ConstZero    | —                           | 把0压入pResult              |
| OP_ConstString  | uint16 constIdx             | 压入字符串常量句柄             |
| OP_VarLocal     | uint16 offset               | 把局部变量读入pResult         |
| OP_Assign       | uint16 dst                  | 把pResult存入局部变量        |
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
| OP_Add_f32  | dst, src          | float同上               |
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

`&&`与`||`没有专用指令——代码生成把它们降级为短路跳转序列
（`OP_JumpIfNot`加双重`OP_LogicalNot`归一化）；实现`!`的是
`OP_LogicalNot`。

### 控制流

| 操作码       | 操作数                | 说明                           |
|--------------|-----------------------|--------------------------------|
| OP_Jump      | int16 target          | 无条件跳转                     |
| OP_JumpIfNot | int16 target, uint16  | 局部变量 == 0时跳转           |

### struct与class

| 操作码            | 操作数                          | 说明                     |
|-------------------|---------------------------------|--------------------------|
| OP_AllocStruct    | dst, structIdx, fieldCount      | 在堆上分配struct        |
| OP_CopyStruct     | dst, src, structIdx             | 深拷贝struct            |
| OP_New            | dst, classIdx                   | 在堆上分配class         |
| OP_LoadField      | dst, obj, fieldOff              | 从堆读取字段             |
| OP_StoreField     | obj, fieldOff, src              | 把字段写入堆             |
| OP_NullCheck      | obj                             | 为null时抛错           |

### 函数调用

| 操作码              | 操作数                      | 说明                     |
|---------------------|-----------------------------|--------------------------|
| OP_CallFunc         | funcIdx, callParamBase      | 调用函数                 |
| OP_CallMethodDirect | funcIdx, callParamBase      | 调用非虚方法             |
| OP_CallMethod       | methodNameIdx, callParamBase| 按名虚分派               |
| OP_CallIntrinsic    | intrinsicId, callParamBase  | 按ID调用内建函数       |

### 函数值

| 操作码              | 操作数                               | 说明                            |
|---------------------|--------------------------------------|---------------------------------|
| OP_MakeFunc         | funcIdx (uint16)                     | 静态句柄 {funcIdx, 0, 0}        |
| OP_MakeBoundFunc    | funcIdx (uint16)                     | 从pResult读取接收者 → {funcIdx, this, static} |
| OP_MakeVFunc        | nameIdx (uint16, 字符串常量表)       | 从pResult读取接收者 → {nameIdx, this, virtual} |
| OP_CallDelegate     | calleeLocal, callParamBase           | 经句柄调用                      |
| OP_CallDelegateOut  | calleeLocal, callParamBase, outMask (uint32) | 另有out写回           |
| OP_Eq_func          | lhs, rhs                             | 句柄内容相等（带null守卫）    |
| OP_Ne_func          | lhs, rhs                             | 句柄内容不等                    |
| OP_Func_to_str      | 累加器形式                           | "func N" / "method N"；null → "<null>" |

句柄布局与分派语义见[一等函数值](first-class-function-values.md#first-class-function-values)。

### 字符串

| 操作码      | 操作数      | 说明                     |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | 拼接字符串               |
| OP_Eq_str   | lhs, rhs    | 字符串相等               |
| OP_Ne_str   | lhs, rhs    | 字符串不等               |
| OP_Less_str | lhs, rhs    | 按字节关系比较（Unicode转换格式（UTF-8，Unicode Transformation Format）字节顺序 == 码点序） |
| OP_LessEqual_str | lhs, rhs | 按字节`<=`              |
| OP_Greater_str | lhs, rhs | 按字节`>`               |
| OP_GreaterEqual_str | lhs, rhs | 按字节`>=`          |
| OP_StrLen   | dst, src    | 字符串长度（字节数） |

### 装箱 / 拆箱 / 向下转型

| 操作码       | 操作数               | 说明                                          |
|--------------|------------------------|----------------------------------------------|
| OP_Box       | typeKind (uint8)       | 把基本类型值（在pResult中）装箱为Object引用；分配RTK_Boxed堆槽位。值为0时短路（保持null）。 |
| OP_Unbox     | typeKind (uint8)       | 从pResult（堆索引）中拆出装箱的基本类型值。校验RTK_Boxed标签是否匹配，不匹配抛错。 |
| OP_CheckCast | classIdx (uint16)      | 校验pResult（堆索引）是classIdx或其子类（沿运行期父类链上溯）。不匹配抛错。引用原样推回。 |

OP_Box/OP_Unbox使用`pResult`寄存器约定——从pResult读入，输出写
回pResult。隐式转换的发射路径（FixupExprType）把基本类型值包进带TCK_Box
的SnCastExpr；显式的`expr as T`运算符创建
SnAsExpr，其代码生成按解析出的转换kind发射OP_Unbox/OP_CheckCast。

### 类型转换

| 操作码           | 说明                     |
|------------------|--------------------------|
| OP_CastIntToFloat | int32 → float           |
| OP_CastFloatToInt | float → int32           |
| OP_Int32_to_str  | int32 → string（经`std::to_string`十进制格式化） |
| OP_Float_to_str  | float → string（`%g`格式） |
| OP_Enum_to_str   | int32枚举值 → string（按名查找） |

字符串强转指令沿用与int/float转换相同的pResult约定：从`pResult`
读取源值，把格式化后的字符串铸造为字符串对象，再把新句柄
（int32）写回`pResult`。发射模式固定为`OP_<type>_to_str`后接
`OP_Assign dst`。

`OP_Enum_to_str`带一个uint16 `enumDefIdx`立即操作数。它从
`pResult`读取int32枚举值，查`m_compiledModule.enumNames[enumDefIdx][value]`，
把名字铸造为字符串对象，再写回句柄。值越界时抛错。

发射位点：
- `EmitExprCast.cpp`的`Access(SnCastExpr&)`按`(srcKind,
  dstKind)`分派int/float→string（隐式强转）。
- `EmitExprMember.cpp`的`Access(SnMemberExpr&)`按
  `outer->EvalDataType()`为`.toString()`调用分派（enum→
  OP_Enum_to_str，int→OP_Int32_to_str，float→OP_Float_to_str，
  string→恒等空操作）。
- `EmitExprCast.cpp`的`Access(SnCastExpr&)`处理二元`+`的
  强转（另一操作数为string时，enum/int/float→string）。

### switch

| 操作码     | 操作数         | 说明                           |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | 标记（无运行期效果）          |
| OP_Case    | uint16 nextOff  | 标记（由编译器回填）          |

switch编译为逐标签比较与条件跳转组成的链——没有跳转表指令。比较指
令按判别式的**家族**（而非静态类型）选择：

- int家族（int与enum，按其int值比较）→ `OP_Equal`（i32）
- float家族 → `OP_Equal_f32`（IEEE `==`：`-0.0 == 0.0`为真，NaN
  永不匹配）
- string家族 → `OP_Eq_str`（按字节内容比较，常量表顺序无关）

多值子句（`case 1, 2:`）为每个标签发射一次比较：每个标签的测试命中
时跳到子句体，未命中跳到下一个标签的测试；单标签子句退化为双
跳转形状。一个子句携带2+N个跳转、四种目标类别（子句出口 / 下一标
签 / 子句体 / 隐式出口），因此跳转回填器（clause-exit fixup，子句出
口修复）遍历的是变长记录列表，而非按定步长定位记录。每个case体都
以一条跳出switch的隐式跳转收尾（Java/C# 式禁止
穿透）；显式`break`还会弹出handler，因为它可能从词法上离开catch
区域。

### 枚举方法调用约定

枚举方法复用类方法调用路径，只有一条约定：**`this`是枚举的int32
值，不是堆引用**。

- 调用在接收者表达式求值进求值认领区（claim area）的槽0之后（接收者
  优先的形状，与`s.equals`相同）发射`OP_CallMethodDirect funcIdx callParamBase`。
- 被调帧的`this`局部变量以typeKind `RTK_Int32`分配（类方法用
  `RTK_Class`）。这一点很关键：垃圾回收（GC，garbage collection）根扫描按typeKind遍历局部变量——
  枚举的`this`若按class建档，就会被当作堆索引追踪并破坏堆。
- 表示决策：枚举在运行期保持nominal int32——`==`、
  `switch`、实参传递与`.ncu`序列化全部不受影响。Java式的堆单例
  枚举需要模块级实例初始化子系统（init函数执行顺序 + GC根），
  被有意推迟。

### 其他

| 操作码       | 操作数         | 说明                     |
|--------------|----------------|--------------------------|
| OP_ParaEnd   | —              | 参数列表结束             |
| OP_DebugInfo | uint16         | 调试行信息               |
