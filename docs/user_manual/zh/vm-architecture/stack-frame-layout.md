# 栈帧布局


每次函数调用都拥有一块局部变量栈帧（stack frame）：

```text
[this (methods)] [params...] [returnSlot] [tempSlot..tempSlot4] [callParamBase(N)] [evalArea(peakDepth)] [user locals...]
```

所有槽位均为4字节（VALUE_SIZE）。栈帧是一块按偏移寻址的扁平字节数组；`LocalDescriptor`记录每个变量的偏移、大小与typeKind，供垃圾回收（GC，garbage collection）根扫描使用。

### 临时槽位

`tempSlot..tempSlot4`是一个4槽纯暂存池：每次使用即弃，绝不跨发射存活。需要跨发射存活的操作数（嵌套调用的实参、成员链的中间接收者等）不进暂存池，而是经`EvalAreaClaim`在求值暂存区认领切片暂存，按词法作用域释放。

### 调用参数区

`callParamBase`起的N个槽位。N是本函数体内所见被调函数形参个数的最大值（最小1），由`ComputeCallSlotStats`在编译期算出。方法调用时槽0是`this`。实参在调用发生前从左到右依次求值并写入该区域。

### 求值暂存区

`evalArea`起的peakDepth个槽位。peakDepth是所有调用点（含嵌套调用）同时需要的暂存槽数最大值，同样在编译期算出。区域整体保持栈式纪律：内层调用的绑定永不覆写外层调用已发射的实参。
