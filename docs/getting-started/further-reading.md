# 下一步阅读

建议按下面的顺序读语言规格：

1. [概览](../language-spec/overview.md)——语言定位与整体结构
2. [命名约定](../language-spec/naming-convention.md)——类型/方法/变量的命名规则
3. [类型](../language-spec/types.md) → [类型语义](../language-spec/type-semantics.md) →
   [声明](../language-spec/declarations.md) →
   [内建泛型类](../language-spec/builtin-generic-classes.md)
   ——类型系统、声明与 List/Dict
4. [表达式](../language-spec/expressions.md) → [语句](../language-spec/statements.md)
   ——运算符、控制流、异常
5. [函数](../language-spec/functions.md) →
   [函数类型与委托](../language-spec/function-types-and-delegates.md)
6. [标准库](../language-spec/standard-library.md)、
   [内存管理](../language-spec/memory-management.md)、
   [退出码约定](../language-spec/exit-code-convention.md)、
   [已知限制](../language-spec/known-limitations.md)

想了解执行引擎：[VM 架构/概览](../vm-architecture/overview.md)，
再按需读编译管线、栈帧布局、字节码指令等章节。

想看能跑的完整程序：`examples/README.md` 按主题列出全部示例与预期退出码，
本页速览片段也大多能在其中找到对应示例。
