> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 规范

本树是NLang编程语言的规范，具有规范性：它覆盖核心语言、标准库，以及实现产生与消费的二进制格式，并定义符合本规范的实现必须做什么。[用户手册](../index.md)讲解如何使用该语言，属资料性；两者在语义问题上分歧时，以本规范为权威。英文文本是规范性的。

本规范描述NLang 0.12.1。它是一份活文档：始终描述当前语言，版本历史就是仓库历史——标签（如`v0.11.5`）把整棵树固定在语言演化的某一个时点上。

本规范与实现不一致时，不一致即其中一方的缺陷：裁定哪一方正确，然后修复那一方。重大语义变更在写入本树之前，先按NLang增强提案（NLEP）流程提出。

## 目录

### 第一部分 — 语言

- [0. 引言](00-introduction.md)
- [1. 词法结构](01-lexical.md)
- [2. 语法](02-grammar.md)
- [3. 程序结构与名称解析](03-program-structure.md)
- [4. 类型](04-types.md)
- [5. 表达式](05-expressions.md)
- [6. 语句](06-statements.md)
- [7. 函数与方法](07-functions.md)
- [8. 面向对象](08-object-orientation.md)
- [9. 异常](09-exceptions.md)
- [10. 执行模型与内存语义](10-execution-model.md)
- [11. 并发](11-concurrency.md)
- [12. 包与导入查找](12-packages.md)

### 第二部分 — 标准库

- [13. 内建类与库约定](13-builtin-classes.md)
- [14. math包](14-math.md)
- [15. io包与流](15-io-streams.md)
- [16. fs包](16-fs.md)
- [17. 并发库](17-concurrency-library.md)
- [18. tensor包](18-tensor.md)
- [19. 库异常](19-library-exceptions.md)

### 第三部分 — 二进制格式

- [20. `.ncu`单元映像](20-ncu-unit-image.md)
- [21. `.npkg`包档案](21-npkg-package-archive.md)
- [22. 原生应用二进制接口（ABI）](22-native-abi.md)
