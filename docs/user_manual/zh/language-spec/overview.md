# 概览


NLang是面向嵌入与自动化场景的静态类型脚本语言，语法类C。它编译为字节码（供虚拟机执行的中间指令形式），由虚拟机执行——一个人工智能（AI，artificial intelligence）友好语言特性的试验台。

关键设计目标：
- 熟悉的C家族语法，学习成本低
- struct值语义、class引用语义（类似C#）
- 确定性、可检视的内存布局（每个值在堆上都有明确位置与类型标记）
- class对象使用标记-清扫垃圾回收
- 经由小型C++应用程序编程接口（API，application programming interface）嵌入宿主应用

### 怎么读这份规格

- [类型](../specification/04-types/index.md)——类型系统一览，枚举/结构体/类/接口/数组等各有专题页
- [声明](../specification/03-program-structure/declarations.md)与[包](../specification/03-program-structure/packages.md)——import、变量、类型别名与模块可见性
- [表达式](../specification/05-expressions/index.md)、[语句](../specification/06-statements/index.md)、[函数](../specification/07-functions/index.md)
- [并发](../specification/10-concurrency/index.md)——虚拟线程、通道、互斥锁、信号量与Future
- [标准库](standard-library.md)
- 运行时：[生命周期与垃圾回收](../specification/09-execution-model/memory.md)与[程序的进入与退出](../specification/09-execution-model/exit-codes.md)
- 参考：[已知限制](known-limitations.md)与[常见错误消息](common-errors.md)

入门顺序建议见[下一步阅读](../getting-started/further-reading.md)。
