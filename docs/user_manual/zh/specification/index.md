> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 规范

本树是NLang编程语言的规范，具有规范性：它覆盖核心语言、标准库，以及实现产生与消费的二进制格式，并定义符合本规范的实现必须做什么；各章在规范性规则之外配有示例与错误说明。语言的系统入门见[NLang入门](../getting-started/what-is-nlang.md)，它属资料性，并把所摘要的规则链接到本规范的对应条款。英文文本是规范性的。

本规范描述NLang 0.13.4。它是一份活文档：始终描述当前语言，版本历史就是仓库历史——标签（如`v0.11.5`）把整棵树固定在语言演化的某一个时点上。

本规范与实现不一致时，不一致即其中一方的缺陷：裁定哪一方正确，然后修复那一方。重大语义变更在写入本树之前，先按NLang增强提案（NLEP）流程提出。

## 目录

### 第一部分 — 语言

- [0. 引言](00-introduction.md)
- [1. 词法结构](01-lexical.md)
- [2. 语法](02-grammar.md)
- [3. 程序结构与名称解析](03-program-structure/index.md)
  - [3.8. 声明](03-program-structure/declarations.md)
  - [3.9. 包与导入查找](03-program-structure/packages.md)
- [4. 类型](04-types/index.md)
  - [4.2. 基本类型](04-types/primitives.md)
  - [4.3. 字符串](04-types/string.md)
  - [4.5. 类型强制转换](04-types/casts.md)
  - [4.6. 枚举](04-types/enum.md)
  - [4.7. 结构体](04-types/struct.md)
  - [4.8. 类](04-types/class.md)
  - [4.9. 接口](04-types/interface.md)
  - [4.10. 数组](04-types/array.md)
  - [4.11. Object与装箱](04-types/object.md)
  - [4.12. 泛型](04-types/generics.md)
- [5. 表达式](05-expressions/index.md)
  - [5.10. 运算符](05-expressions/operators.md)
  - [5.11. 集合初始化器](05-expressions/initializers.md)
- [6. 语句](06-statements/index.md)
  - [6.7. 控制流](06-statements/control-flow.md)
  - [6.8. 循环](06-statements/loops.md)
- [7. 函数](07-functions/index.md)
  - [7.2. 参数与实参](07-functions/parameters.md)
  - [7.3. 重载解析](07-functions/overloads.md)
  - [7.4. 函数值与委托](07-functions/function-values.md)
  - [7.5. 原生函数](07-functions/native.md)
- [8. 异常](08-exceptions/index.md)
- [9. 执行模型](09-execution-model/index.md)
  - [9.2. 生命周期与垃圾回收](09-execution-model/memory.md)
  - [9.3. 程序的进入与退出](09-execution-model/exit-codes.md)
- [10. 并发](10-concurrency/index.md)
  - [10.8. 载体与并行](10-concurrency/carriers.md)
  - [10.9. 挂起与调度限制](10-concurrency/suspension.md)

### 第二部分 — 标准库

- [11. 库约定](library/11-conventions.md)
- [12. 集合](library/12-collections.md)
- [13. math包](library/13-math.md)
- [14. io包与流](library/14-io/index.md)
  - [14.6. ByteStream与FileStream](library/14-io/streams.md)
- [16. fs包](library/16-fs.md)
- [17. string方法](library/17-string-methods.md)
- [18. 并发类](library/18-concurrency-classes/index.md)
  - [18.3. thread包](library/18-concurrency-classes/thread.md)
  - [18.4. Future](library/18-concurrency-classes/future.md)
  - [18.5. Promise](library/18-concurrency-classes/promise.md)
  - [18.6. Mutex](library/18-concurrency-classes/mutex.md)
  - [18.7. Semaphore](library/18-concurrency-classes/semaphore.md)
  - [18.8. ReentrantMutex](library/18-concurrency-classes/reentrant-mutex.md)
  - [18.9. AtomicInt](library/18-concurrency-classes/atomic.md)
  - [18.10. Channel](library/18-concurrency-classes/channel.md)
  - [18.11. concurrent包](library/18-concurrency-classes/concurrent.md)
- [19. tensor包](library/19-tensor.md)
- [20. 库异常](library/20-library-exceptions.md)
- [21. 网络库](library/21-network/index.md)
  - [21.2. 传输控制协议（TCP）类](library/21-network/tcp.md)
  - [21.3. UdpSocket类](library/21-network/udp.md)
  - [21.4. 互联网控制报文协议（ICMP）面](library/21-network/icmp.md)

### 第三部分 — 二进制格式

- [23. `.ncu`单元映像](formats/23-ncu.md)
- [24. `.npkg`包档案](formats/24-npkg.md)
- [25. 原生应用二进制接口（ABI）](formats/25-abi.md)

### 参考

- [已知限制](reference/known-limitations.md)
- [常见错误消息](reference/common-errors.md)
- [命名约定](reference/naming-convention.md)
