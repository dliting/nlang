# 已知限制

本页汇总当前对用户程序可见的限制，并给出可行的替代做法；语言层面的限制见[已知限制](../specification/reference/known-limitations.md)。

1. **退出码范围**：进程退出码以32位原值传递；但POSIX shell的`$?`只保留低8位（退出码300在bash下被观察为44）。测试预期值保持 ≤ 255，以便跨观察者一致（详见[程序的进入与退出](../specification/09-execution-model/exit-codes.md)）。
2. **struct没有方法**：struct只承载数据。需要行为时请用class。
3. **`List<int>`存储开销**：每个基本类型元素都装箱进一个堆槽位（`RTK_Boxed`）。值密集的场景请改用`int[]`——数组存储是原生的（一次连续分配容纳全部元素单元，没有逐元素装箱）。
