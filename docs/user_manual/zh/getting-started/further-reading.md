# 下一步阅读

建议按下面的顺序读[语言规范](../specification/index.md)：

1. [类型](../specification/04-types/index.md) →
   [声明](../specification/03-program-structure/declarations.md) →
   [泛型](../specification/04-types/generics.md)
   ——类型系统、声明与List/Dict
2. [表达式](../specification/05-expressions/index.md) → [语句](../specification/06-statements/index.md)
   ——运算符、控制流、异常
3. [函数](../specification/07-functions/index.md) →
   [函数值与委托](../specification/07-functions/function-values.md)
4. [并发](../specification/10-concurrency/index.md)——`thread.spawn`派生虚拟线程，通道、互斥锁与Future
5. [内建类与库约定](../specification/13-builtin-classes.md)——标准库一览，其后是各包的章节
6. [生命周期与垃圾回收](../specification/09-execution-model/memory.md)与[程序的进入与退出](../specification/09-execution-model/exit-codes.md)

参考页——[命名约定](../language-spec/naming-convention.md)、[已知限制](../language-spec/known-limitations.md)与[常见错误消息](../language-spec/common-errors.md)——是规范的资料性补充。

想了解执行引擎：[VM架构/概览](../vm-architecture/overview.md)，再按需读编译管线、栈帧布局、字节码指令等章节。五个命令行工具（nlang/ncc/nvm/ndb/ndisasm）的用法见[命令行工具](../cli-tools/overview.md)一章。在集成开发环境（IDE，integrated development environment）里调试程序见[在nide中调试](debugging.md)。

想看能跑的完整程序：`examples/README.zh-CN.md`按主题列出全部示例与预期退出码，本页速览片段也大多能在其中找到对应示例。

想扩展或嵌入：[开发第三方库](../libraries/developing-libraries.md)讲怎么把可复用代码做成库（含native混合形态），[集成NLang](../libraries/integrating-nlang.md)讲怎么在自己的程序里以子进程驱动NLang——依赖清单、部署与调试。
