# 概览


NLang是面向嵌入与自动化场景的静态类型脚本语言，语法类C。它编译为字节码、由基于寄存器的VM执行——一个人工智能（AI，artificial intelligence）友好语言特性的试验台。

关键设计目标：
- 熟悉的C家族语法，学习成本低
- struct值语义、class引用语义（类似C#）
- 确定性、可检视的内存布局（堆槽位、带类型的kind）
- class对象使用标记-清扫垃圾回收
- 经由小型C++应用程序编程接口（API，application programming interface）嵌入宿主应用
