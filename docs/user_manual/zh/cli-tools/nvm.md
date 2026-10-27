# nvm —— 运行程序

nvm运行一个已编译的产物——`.ncu`单元映像或`.npkg`程序包，不经过编译步骤。场景：部署或自动化环境中只执行不编译；反复运行同一程序省去重复编译；脚本与流水线中以退出码判定结果。

```text
nvm <program.ncu|.npkg> [-I <dir>...] [--verbose | -v] [--gc-stress=N] [--carriers=N]
```

运行一个编译好的程序，进程退出码 = `main`返回值（约定详见[退出码约定](../language-spec/exit-code-convention.md)）。产物文件打不开时报`Runtime error: nloader failed:`（诊断正文逐行列出问题，如`'<路径>': Failed to open module file: <路径>`）。`--gc-stress=N`是测试旋钮：把两套垃圾回收（GC，garbage collection）阈值钳到极小值，任何漏追踪的引用会在几次分配内变悬垂——用于验证内存管理变更，日常使用不需要。该标志写在程序路径之前或之后均可。`--carriers=N`设定调度器载体线程数（1..64）：`nvm`默认至多4个（不超过机器并行度），`--carriers=1`钉为单载体以获得确定性交错（见[GIL与多载体](../language-spec/concurrency-carriers.md)）；范围外的值在程序启动前被拒绝。

产物自身不含库代码：加载器先沿搜索路径发现全部依赖（标准库包、外部`.ncu`/`.npkg`），再链接成唯一的运行期模块执行。缺包时报`Runtime error: nloader failed:`，正文形如`module '...' not found (searched: <目录>, ...)`，一次列出已搜索的全部目录——先检查`-I`与`NLANG_PATH`（机制详见[ncc](ncc.md)的「产物与加载期链接」）。

`-I <dir>`追加库搜索目录（可多次指定）。运行期按一组有序目录定位全部依赖成员与native动态库：`-I`目录 → 模块所在目录 → 环境变量`NLANG_PATH`（Windows以`;`、POSIX以`:`分隔）→ 可执行文件目录 /当前目录 → 标准库目录（`stdlib.npkg`所在处）；前面的目录优先，重复目录只保留第一次出现。完整规则见[库与搜索路径](../language-spec/stdlib-search-paths.md)。

## 并发与载体

`--carriers=N`设定执行本程序虚拟线程的操作系统线程数（1..64，越界直接拒绝）。不写时nvm取4与机器硬件并发数中的较小值。`--carriers=1`完全确定：严格的先进先出轮转，同一程序同一输入每次运行产生相同的交错，测试可以放心断言。载体大于1时交错不指定：程序正确性必须建立在同步原语（`join`、`Channel`、`Mutex`、`Future.get`）之上，绝不能依赖时序。载体数改变的是交错，不是结果：

```text
nvm --carriers=1 program.ncu    # 交错可复现
nvm --carriers=8 program.ncu    # 结果相同，交错不同
```

多载体到底并行了什么，见[载体与并行](../language-spec/concurrency-carriers.md)；安装包自带的`examples/conc_*.n`演示了这个标志。

`--verbose`（短写法`-v`）在程序运行前打印解析后的搜索路径——每行一个目录、按搜索顺序排列，行尾括注其来源层（`(-I)`、`(local directory)`、`(NLANG_PATH)`、`(system)`）——随后照常执行。遇到上面的「缺包」诊断时，给失败的调用加上`--verbose`重跑一遍，即可看到将要搜索的全部目录。
