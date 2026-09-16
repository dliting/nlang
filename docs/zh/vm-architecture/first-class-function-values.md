# 一等函数值（Phase 13）

<a id="first-class-function-values-phase-13"></a>

函数值（function value，即委托 delegate）是一个 **3 槽堆记录**，满足
`m_slotKinds[idx] == RTK_Func`：

| 槽位 | 静态绑定句柄（`kFuncFormStatic`） | 虚分派句柄（`kFuncFormVirtual`） |
|------|------------------------------------------|----------------------------------------------|
| [0]  | `functions[]` 索引                       | string 池索引（方法名）                      |
| [1]  | 接收者堆索引（自由函数为 0）             | 接收者堆索引                                 |
| [2]  | 形式（0 / 1）                            | 形式（0 / 1）                                |

**不变量**：`slot[1] == 0` ⟺ 自由函数形式。OP_MakeBoundFunc /
OP_MakeVFunc 绑定时的空接收者守卫（接收者槽位读到 ≤ 0 时抛出空指针
异常）确立了它：空接收者永远无法铸造出带着垃圾栈帧走上自由函数分派
路径的句柄。

### 分派布局（`this == 0` 分支）

`ExecuteDelegateCall`（OP_CallDelegate / OP_CallDelegateOut 共用）按
`slot[1]` 分支：

- **自由函数**——实参从 `callParamBase` 原样拷贝（OP_CallFunc 的
  ABI）。原生目标直接读取调用方的槽位单元。
- **绑定方法**——被捕获的接收者占据被调帧的槽 0，调用方的实参
  *不带* this 依次落位，从槽 1 开始（`callee.paramCount` 对方法而言
  包含 this）。
- **虚分派句柄**——先解析目标：在接收者的运行期类上按方法名查找
  （`FindMethodByName`，与 OP_CallMethod 执行同一套查找，沿
  `superClassIdx` 链向上遍历 `methodIndices`），再按绑定方法布局执
  行。落到此处的原生或内建方法在分派时抛错（可编译的形状已被
  resolver 拒绝）。

out 实参写回（`OP_CallDelegateOut`）按相反方向平移：outMask 的第
*i* 位标记 Func 签名顺序中的第 *i* 个用户参数；写回时对绑定句柄读
取帧槽位 `i+1`（自由函数为槽 `i`），存入 `callParamBase + i`。

### 接收者优先发射

OP_MakeBoundFunc / OP_MakeVFunc 在写入句柄*之前*从 `pResult` 读取接
收者：代码生成先把接收者表达式发射进结果槽位，刷新 `pResult`，然后
绑定指令读取它，再用句柄索引覆写该槽位。

### GC 追踪位点

MarkPhase 按 kind 为追踪位点设门——多数看运行期 kind，两个根扫描看
静态局部/返回 kind；`RTK_Func` 记录在 **8 个** 位点被追踪（Phase 13
把分支补齐到了所有位点）：

1. 根集扫描（`LocalDescriptor.typeKind == RTK_Func` 的局部/参数）
2. 栈帧 `pResult` 扫描（进行中的返回值）
3. 堆对象弹出分派（`slot[1]` 里的接收者）
4. class 字段扫描（静态 `fieldTypeKinds` + 运行期 kind）
5. struct 字段扫描（struct 赋值时的浅拷贝是正确语义——句柄是引用
   而非值）
6. 数组元素扫描（`elemKind`）
7. List 元素扫描
8. Dict 条目扫描

句柄从不进入装箱槽位（装箱槽位不被追踪）。分配总是发生——没有驻留
（interning）——因此指向同一函数的两个引用是两条按内容比较
（`OP_Eq_func`）的独立记录。
