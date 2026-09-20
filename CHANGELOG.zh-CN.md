# 更新日志

**中文** | [English](CHANGELOG.md)

这里记录 NLang 的所有重要变更。格式遵循
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；版本
遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [0.8.0] - Unreleased

### 变更
- 运行期字符串成为可被垃圾回收的不可变对象：长时运行的程序（持续
  构建提示词的循环等）不再无界增长内存，`s = s + x` 追加为 O(1)
  （透明拼接节点，首次读取时展平）而非 O(n) 整串拷贝。

### 性能
- 运行期创建的不超过 40 字节的短串按内容驻留重用；驻留串之间的
  `==` 比较退化为句柄比较。

### 修复
- `List.indexOf`/`contains` 现按内容比较字符串元素；与已存元素内容
  相同的拼接结果或驻留串现在能够匹配。
- `Exception.backtrace.get(i)` 不再抛出 "unbox on null/invalid
  reference"；帧条目可正常读取。

## [0.7.0] - 2026-09-19

### 新增

- nide「工具 → 选项」对话框：界面语言（跟随系统 / 中文 / 英文，
  重启后生效）与全局构建输出目录（独立 `.nmod` 文件落到那里；
  `.nproj` 未设置输出目录时项目回退到它）。
- 手册全面双语化：文档站构建两棵完整的树（`zh/` + `en/`），由
  语言探测落地页引导并带跨树切换链接，nide 帮助菜单按语言设置
  打开对应树。
- 根部文档双语化：`README.zh-CN.md` 与 `CHANGELOG.zh-CN.md` 与
  英文原版一一对应（各文件顶部互链），并随发布包分发。
- nide：构建输出目录未设置时，「工具 → 选项」的输入框以占位符
  显示缺省位置（`%TEMP%\nlang-nide`），「浏览」也从那里启动。

## [0.6.2] - 2026-09-15

### 新增

- 泛型类型实参可以是数组类型（`List<int[]>`、`Dict<int[], int>`）：
  实例化键逐实参携带数组性，数组类型的元素以原始 GC 追踪句柄
  （而非装箱的基本类型）流转，`Func` 签名匹配与 `Dict.keys()` 在
  擦除边界两侧保持数组性。
- `foreach` 与 `for` 循环变量可以是数组类型
  （`foreach (int[] row in grid)`）；循环变量类型必须与元素类型
  精确匹配——同字段同数组性，无数值加宽
  （`foreach_var_mismatch_reject`、`foreach_var_widen_reject`）。
- 数组元素指令（`OP_LoadElement`、`OP_StoreElement`、
  `OP_ArrayLength`）在运行时校验基槽 kind，失败时给出具名诊断，
  而非读取悬垂句柄。

### 变更

- `.nmod` 格式下限 v1.10 → v1.11：泛型容器元素存储的语义变更
  （原始追踪句柄、基本类型不装箱）；较旧模块因过期被拒绝，须
  重新编译。

## [0.6.1] - 2026-09-12

### 变更

- README、CONTRIBUTING 与文档站的定位重写：NLang 被描述为一门
  面向嵌入与自动化的静态类型脚本语言——精简的 C++ 宿主 API、
  原生绑定与进程内调试钩子——以及 AI 友好语言特性的试验台。
  对私有前代代码库的全部引用已从分发源码与文档中移除。
- 入门 `switch` 示例不再从每个 case 分支 return；状态累积形式让
  「无穿透」语义可见。

### 新增

- 公开文本守卫：单一模式源扫描每个被跟踪文件（ctest
  `nlang_docs_pytest`）与每个发布包（`verify_package.py`），前代
  引用重现于公开散文时使构建或发布失败。

## [0.6.0] - 2026-09-12

### 新增

- 数组值表达式携带 resolve-time 类型属性；交错数组声明（`T[][]`）
  与非容器 `foreach` 源在编译期以具名诊断拒绝。
- GC 追踪数组类型元素槽中持有的数组记录。

### 变更

- `.nmod` 格式下限提升到 v1.10：数组的 struct/class 字段现在以
  `RTK_Array` 作为字段 kind（此前是元素 kind）；较旧模块须重新
  编译。
- 流式写入含数组字段的 struct（`bs.writeStruct`）现在抛出具名
  错误，而非静默写入原始堆句柄。

### 修复

- `int[]` 类型的 class 字段不再破坏 `toString()` 分派。
- `.length` 在任意数组值接收者上解析（`li.get(0).length`、
  `lib.mk(3).length`），不再仅限标识符局部/字段。

## [0.5.0] - 2026-09-09

### 新增

- nide：调试套件。F5 启动会话——程序先构建，然后运行到首个断点
  或运行到底——再按一次 F5 继续；Shift+F5 随时停止会话（硬终止，
  任何情况都有效，包括死循环或原生代码内部）。F9 或行号槽点击
  切换断点（实心点 = 实会话中已绑定，空心 = 未绑定）；断点跨重启
  持久化并跟随文件重命名。F10/F11/Shift+F11 单步跳过/进入/跳出。
  输出区新增「调试」页，显示会话状态、抛异常时中断开关、调用栈
  （点击帧选中、跳转到该行并刷新局部变量）与所选帧的局部变量；
  停驻行在编辑器中以行号槽箭头高亮。程序输出与错误回溯流式写入
  「运行输出」页。会话存活期间构建/运行被禁用，关闭 nide 终止被
  调试进程。
- VM：`IHostIo`——面向嵌入式前端的宿主 I/O 接缝：输出字节经回调
  原样到达，声明无输入的已安装宿主使 `io.readLine` 抛出可捕获的
  IOException，而非静默消费嵌入者的流。未安装宿主（默认）时控制
  台行为不变，ncc、nvm 与 CLI 调试器不受影响。
- ndb：`--machine` 模式——stdin/stdout 上的行协议，供 IDE 嵌入
  （tab 连接的事件与转义字段：hello/bp/stopped/frame/local/done/
  output/exited/error/err；断点在 `run` 之前设置）。

### 变更

- VM：while/for/do-while 的行断点与步进现在每次迭代都命中——
  回边落在锚点上（while/for 的条件入口、do-while 的尾条件；gdb
  语义）。此前锚点只在循环入口触发一次，空循环体没有逐迭代
  检查点。
- ndb：断点身份变为每源码行一个 id——一行携带多个语句锚点
  （如循环头）时合并到单一断点 id 下，断点的设置、删除与报告在
  CLI 与机器模式中行为一致。
- nide：运行 → 开始运行 从 F5 移到 Ctrl+F5；F5 现在启动（并
  继续）调试器。

## [0.4.0] - 2026-09-07

### 新增

- ndb：已编译模块的 CLI 调试器。断点（`b <file.n:LINE | LINE |
  funcName>`）、继续、单步进入/跳过/跳出、回溯、帧选择、
  `info locals`、`p`、源码列表 `l`、反汇编 `x`、`catch on|off`
  （抛异常时中断）。像 gdb `start` 一样在首条语句初始停驻；
  stdin EOF 等同 `q`；ndb 以被调试程序的退出码退出。
- VM：进程内调试钩子（`IDebugHooks`——语句与抛出检查点），加
  只读的冻结状态视图（`IVmDebugView`）；前端无关的接口，将来的
  DAP 适配器或 IDE 可复用。反汇编打印抽取为共享的 `Disassembler`
  （ndisasm 输出逐字节一致）。
- `.nmod` v1.9：每个函数记录其源文件路径（跨文件断点寻址）。
  import 合并同时拷贝 `func.locals`，修复了导入帧根集为空、活对象
  可能被清扫的既有 GC 根集洞。

### 变更

- `.nmod` 格式下限从 8 提升到 9：较旧模块被加载器拒绝，须重新
  编译。

## [0.3.0] - 2026-08-31

### 新增

- nide：关于对话框现在显示项目 GitHub 地址
  （https://github.com/dliting/nlang），为蓝色下划线链接；点击
  打开默认浏览器。

### 变更

- 语言：`&&` 与 `||` 现在短路求值（被跳过的操作数从不求值——
  无副作用、无抛出），与 C/C++/Java/Python 惯例一致；结果保持
  `int` 的 `0`/`1`。`&&`、`||` 与 `!` 的操作数现在必须是 `int`
  （float/string 操作数此前被按原始比特读取、真值无意义——现在
  是编译错误）。包含已移除的急切求值 `OP_LogicalAnd`/
  `OP_LogicalOr` 指令的旧字节码模块须重新编译。

## [0.2.0] - 2026-08-31

### 新增

- nide：**文件 > 最近打开** 子菜单——最近的解决方案、项目与文件，
  按最近使用优先排序，跨会话持久化。同名条目以父目录消歧并带
  完整路径的工具提示；打开、新建、另存为与重命名都会进账。
- 版本管理：仓库根部的 `VERSION` 文件成为版本的单一来源——
  `ncc`/`nvm`/`ndisasm --version`、IDE 的关于对话框、文档站页脚
  与包名全部由它派生。本更新日志是该工作流的一部分。

## [0.1.0] - 2026-08-30

首次公开发布。

### 新增

- 语言：一门静态类型脚本语言——支持继承与 `super()` 的 class 与
  struct、函数、方法与代理、类型别名、数组、`List`/`Dict`、
  `foreach`、`switch`/`enum`、异常处理（`try`/`catch`/`finally`/
  `throw`，含内建异常类）、字符串插值、增量赋值、`assert` 与
  `const` 局部。
- 工具链：`ncc`（编译并运行；单文件与 `.nproj` 项目）、`nvm`
  （字节码运行器）、`ndisasm`（字节码反汇编器）。
- nide：Qt5 IDE——含独立文件的解决方案树、编辑器、构建与运行、
  内嵌离线文档查看器、中英文界面。
- 标准库：`math`/`io`/`fs` 命名空间与内建字符串方法。
- 跨文件编程：显式 `import`（单段、通配与预编译 `.nmod` 模块三种
  形式）。
- 文档：完全离线可用的文档站与可运行的 `examples/`。
- Windows 打包：便携 zip 与 NSIS 安装程序。

[0.7.0]: https://github.com/dliting/nlang/compare/v0.6.2...v0.7.0
[0.6.2]: https://github.com/dliting/nlang/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/dliting/nlang/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/dliting/nlang/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/dliting/nlang/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/dliting/nlang/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/dliting/nlang/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/dliting/nlang/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/dliting/nlang/releases/tag/v0.1.0
