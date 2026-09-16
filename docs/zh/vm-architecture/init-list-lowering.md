# 初始化列表降级（Phase 8e-6）


集合初始化器（`[1,2,3]`、`new Type{...}`）经
`VmBackend::EmitExpression` 中单一的 `NK_InitListExpr` 处理器降级。
该处理器按解析出的 `EvalDataType` 加上 resolver 设置的
`TargetIsArray` 标志分派。

### resolver 数据流

`StatementResolver::Access(SnAssignStmt&)` 在解析右值之前先做前瞻
（peek）。若右值是不带 `ExplicitType` 的裸 `SnInitListExpr` 且没有
`InferredTarget`，就把左值变量经 `InferredTarget(pLeftField)` 记录
到该初始化列表上。随后 resolver 访问右值；
`ExprResolveAccessor::Access(SnInitListExpr&)` 读取显式的
`ExplicitType()` 或 `InferredTarget()`，把 `EvalDataType` 设为解析
出的目标字段，并把数组性拷贝进 `TargetIsArray`（这一步必不可少：数
组变量上的 `EvalDataType` 返回的是元素类型，否则数组性会丢失）。

resolver 不向子初始化列表传播期望类型（`[[1,2],[3]]` 的递归类型推断
暂不支持）。子列表必须使用显式的 `new Type{...}` 形式自带类型。

### 代码生成分派表

| 目标 kind                    | 分配                   | 逐条目存储                           |
|-----------------------------|------------------------|--------------------------------------|
| `T[]` 数组                  | `OP_AllocArray` size=N | `OP_ConstInt32 i; OP_StoreElement`  |
| `List<T>`                   | `OP_New "List"` + ctor | `OP_Box`（T 为基本类型时）；`OP_CallMethod "Add"` |
| `Dict<K,V>`                 | `OP_New "Dict"` + ctor | `OP_ConstString key`；`OP_Box`（V 为基本类型时）；`OP_CallMethod "Set"` |
| 用户 class                  | `OP_New` + 无参 ctor   | `OP_StoreField <offset>`            |
| struct                      | `OP_AllocStruct`       | `OP_StoreField <offset>`            |

### 逐方法装箱计划复用

`List<T>` 与 `Dict<K,V>` 分支复用 Phase 8e-3/8e-4 的逐方法装箱基础
设施：`BoxingTagFor(typeArg)` 返回 `{tag, isPrimitive}`，只有当元素
类型为基本类型**且不是数组类型**时才在 `Add`/`Set` 之前发射 `OP_Box`
——数组类型的类型实参（`List<int[]>`、`Dict` 的值 `V[]`）以裸句柄
流动、不做装箱，与手写 `lst.add(x)` 调用的例外一致。这保证初始化与
手写的 `lst.add(x)`、`d.set(k, v)` 调用行为一致。

### 临时槽位分配

处理器把集合写入 `resultOffset`（由调用方提供，例如 struct 赋值代
码生成里的 `tempSlot2`）。每个条目的值都求值到
`PickTempSlot(resultOffset)` 挑出的独立槽位，嵌套初始化列表（会递归
进入本处理器）因此不会覆写父级的值槽位。

### 复用既有指令

Phase 8e-6 没有新增任何指令。分配、元素存储、方法调用与字段存储全
部复用 Phase 3/4/8e-3/8e-4 的原语。初始化列表处理器纯粹是编排：先
分配一次，再用既有存储指令填充。
