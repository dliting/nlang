# 栈帧布局


每次函数调用都拥有一块局部变量栈帧（stack frame）：

```text
[this (methods)] [params...] [returnSlot] [tempSlot] [tempSlot2] [callParamBase(8 slots)] [user locals...]
```

所有槽位均为 4 字节（VALUE_SIZE）。栈帧是一块按偏移寻址的扁平字节
数组；`LocalDescriptor` 记录每个变量的偏移、大小与 typeKind，供垃圾
回收（GC）根扫描使用。

### 临时槽位

- **tempSlot**：存放二元运算的右操作数、条件求值结果，以及成员访问
  链中的中间值。
- **tempSlot2**：嵌套二元表达式的第二个临时槽。当 resultOffset ==
  tempSlot 时，右操作数改入 tempSlot2，避免覆盖已存的结果。

### 调用参数区

`callParamBase` 处的 8 个槽位（32 字节）。方法调用时槽 0 是 `this`。
实参在调用发生前从左到右依次求值并写入该区域。
