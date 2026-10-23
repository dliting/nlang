# 初始化列表降级

集合初始化器让开发者用一条字面量表达式直接给数组或容器填初值，例如`[1,2,3]`或`Type{...}`。这些字面量没有专门的运行期形态：编译器把它们降级为普通的分配与存储指令。本页说明降级路径、目标类型的推断方式，以及容器分支的装箱处理。

集合初始化器（`[1,2,3]`、`Type{...}`）经`VmBackend::EmitExpression`中单一的`NK_InitListExpr`处理器降级。该处理器按解析出的`EvalDataType`与节点的`IsArrayValued()`谓词分派——数组形态初始化器的类型槽携带驻留数组令牌，数组性即类型本身。

### resolver数据流

`StatementResolveAccessor::Access(SnAssignStmt&)`在解析右值之前先做前瞻（peek）：不带`ExplicitType`的裸初始化列表把左值字段经`InferredTarget(pLeftField)`记录到节点上——成员存储（`b.m = {...}`）同样如此。所有能提供目标的位置都以同样方式传播：裸花括号返回值取函数声明的返回类型，下标存储的右值取元素/容器值槽位类型，调用实参取形参类型，foreach的可迭代对象取循环变量类型（花括号形态迭代由其铸造的`List<T>`，方括号形态取驻留数组令牌）。

随后resolver访问右值；`ExprResolveAccessor::Access(SnInitListExpr&)`读取显式的`ExplicitType()`或`InferredTarget()`——两者皆无时，方括号形态字面量从条目推断元素类型（所有条目必须共享同一类型）——并把`EvalDataType`设为解析出的目标类型：数组目标为驻留数组令牌（`InternArrayTypeToken(元素)`），节点因此天然为数组值（`IsArrayValued()`），消费者经`ElemTypeOf()`剥出元素。

resolver按槽位把元素目标传播进子初始化列表（`PropagateNestedInitListTargets`），嵌套裸字面量可推断类型——`[[1,2],[3]]`与`{{1,2},{3}}`都可用。

### 代码生成分派表

| 目标kind                    | 分配                   | 逐条目存储                           |
|-----------------------------|------------------------|--------------------------------------|
| `T[]`数组                  | `OP_AllocArray` size=N | `OP_ConstInt32 i; OP_StoreElement`  |
| `List<T>`                   | `OP_New "List"` + ctor | `OP_Box`（T为基本类型时）；`OP_CallMethod "add"` |
| `Dict<K,V>`                 | `OP_New "Dict"` + ctor | 键与值均为表达式；`OP_Box`（K/V为基本类型时）；`OP_CallMethod "set"` |
| `Channel<T>`                | `OP_New "Channel"` + ctor（无界容量以常量暂存） | —（resolver拒绝条目；值经`send`进入） |
| 用户class                  | `OP_New` + 无参ctor   | `OP_StoreField <offset>`            |
| struct                      | `OP_AllocStruct`       | `OP_StoreField <offset>`            |

### 逐方法装箱计划复用

`List<T>`与`Dict<K,V>`分支复用容器调用的逐方法装箱基础设施：`BoxingTagFor(typeArg)`返回`{tag, isPrimitive}`，只有当元素类型为基本类型**且不是数组类型**时才在`add`/`set`之前发射`OP_Box`——数组类型的类型实参（`List<int[]>`、`Dict`的值`V[]`）以裸句柄流动、不做装箱，与手写`lst.add(x)`调用的例外一致。这保证初始化与手写的`lst.add(x)`、`d.set(k, v)`调用行为一致。

### 条目值的暂存

处理器把集合写入`resultOffset`（由调用方提供，例如struct赋值代码生成里的`tempSlot2`）。每个条目的值都求值到求值暂存区的暂存槽位：数组、class与struct分支以一次`EvalAreaClaim`贯穿整个条目循环；List与Dict分支按条目认领`{this, 值}`与`{this, 键, 值}`槽位。条目求值后立即被消费；嵌套初始化列表（会递归进入本处理器）在其上认领新槽位，因此不会覆写父级的值槽位。

### 复用既有指令

初始化列表降级没有新增任何指令。分配、元素存储、方法调用与字段存储全部复用既有的原语指令。初始化列表处理器纯粹是编排：先分配一次，再用既有存储指令填充。
