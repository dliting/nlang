# NLang是什么

NLang是一门面向嵌入与自动化场景的静态类型脚本语言，也是探索人工智能（AI，artificial intelligence）友好语言特性的试验台，自带编译器（ncc）、字节码虚拟机（nvm，执行ncc输出的字节码——编译器产出的指令形式）与集成开发环境nide（IDE，integrated development environment），作为教学/研究开源项目开发。

上手路线：[安装与布局](install-layout.md)装好工具链，[五分钟上手](first-program.md)跑通第一个程序，再从[速览：类型与变量](types-and-variables.md)开始用可运行的片段过一遍语言核心特性。本文也是nide帮助菜单「NLang入门」的落地页。

源码（`.n`）先编译为字节码产物——单文件是`.ncu`单元映像，多文件项目是`.npkg`程序包——执行时由虚拟机把产物与其依赖（标准库包、外部模块）装载链接成运行期模块。语言本身：

- **静态类型**——变量、参数、返回值都显式声明类型，没有类型推断；
- **面向对象**——`class`/继承/接口/虚分派，另有struct（值类型）与enum；
- **内建泛型集合**——`List<T>`/`Dict<K,V>`，带初始化器语法；
- **垃圾回收**——没有`delete`，对象在不可达后自动回收，无需手动释放内存（见[内存管理](../language-spec/memory-management.md)）；
- **并发**——`thread.spawn`把函数放到自己的线程上运行，返回`Future<T>`承载结果；`concurrent`包（`Mutex`、`Channel`、`Semaphore`、`WaitGroup`）构成同步层；
- **现代脚本语言标配**——异常处理、字符串插值、`out`参数、默认参数、`math`/`io`/`fs`/`thread`/`concurrent`标准库。

随包分发的六个工具：

| 工具 | 角色 |
|---|---|
| nlang | 脚本直跑器：免`main()`样板直接运行`.n`脚本或`-c`内联语句，进程内编译执行，不在磁盘留下产物 |
| ncc | 命令行编译器：把`.n`源文件编译成`.ncu`、把`.nproj`项目编译成`.npkg`，也可编译后立即执行 |
| nvm | VM执行器：运行`.ncu`与`.npkg` |
| ndb | 命令行调试器：断点、单步、调用栈（nide里的图形调试也由它驱动） |
| ndisasm | 字节码反汇编器：查看`.ncu`/`.npkg`里的指令 |
| nide | IDE：编辑、构建、运行，内嵌本帮助站 |

六个工具的完整用法见[命令行工具](../cli-tools/overview.md)一章；nide的图形界面用法见[三种运行方式](running.md)与[在nide中调试](debugging.md)。

