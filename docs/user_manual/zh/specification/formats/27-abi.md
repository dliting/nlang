> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 27. 原生应用二进制接口（ABI）

*原生模块*是一个共享库，它依据原生应用二进制接口（ABI）实现原生函数——§7.5中声明为`native`、经限定键`<ns>.<name>`调用的那些函数。标准库模块与第三方模块使用同一界面。§27.1界定范围并固定ABI版本；§27.2固定模块发现与加载；§27.3固定模块导出与注册；§27.4固定调用约定与值单元；§27.5定义宿主函数表；§27.6定义固定数组视图；§27.7陈述阻塞段契约及其上的输入输出（IO）停泊。

## 27.1 范围与版本

**ABI版本。**本章描述ABI版本 **11**。宿主接受版本探针恰好报`11`的模块，并拒绝其他一切值，包括根本不导出探针的模块（§27.2）。ABI变更会提升版本并整章替换本章；规范只描述当前版本。

**本章固定的内容。**模块发现与加载（§27.2）、模块导出与注册（§27.3）、调用约定与值单元（§27.4）、宿主函数表（§27.5）、固定数组视图（§27.6）与阻塞段契约及其上的IO停泊（§27.7）。

**本章不固定的内容。**语言侧的`native`声明、它的按键方式与实参规则（§7.5）；单元映像内部的指令编码（第25章）；以及宿主不经模块、直接提供原生实现所用的嵌入接口——这样的宿主实现在任何模块查找之前应答，受调用约定与宿主函数表约束，但不受模块导出约束。

**无状态性。**原生模块不保持任何语言可观察的状态：伪随机数生成器（PRNG）与控制台住在宿主里，只能经宿主函数表到达（§27.5）。

## 27.2 模块发现与加载

**模块文件。**实现命名空间`<ns>`的模块是一个共享库，在Windows上名为`nlang_<ns>.dll`，在macOS上名为`libnlang_<ns>.dylib`，在其他POSIX系统上名为`libnlang_<ns>.so`。

**惰性加载。**在键`<ns>.<name>`尚未注册的首次调用上，宿主载入该命名空间的模块——命名空间就是第一个点号之前的段（§7.5）。不带点号的键是裸名，绝不触发模块加载。每个命名空间只加载一次；同一命名空间中所有后续调用复用已加载的模块。

**搜索顺序。**宿主在一个有序目录列表中搜索模块文件。正在运行的可执行文件所在目录总在列表中；嵌入宿主可以添加目录，开发构建会追加存放其自身原生产物的构建树目录。在任何目录都找不到的模块，在调用处抛出宿主级执行期错误（§8.1），并命名该文件与每个被查询过的目录。

**加载序列。**宿主打开模块文件，然后：

1. 解析导出`nlang_abi_version`；没有该导出的模块被拒绝；
2. 调用它；除`10`以外的值被拒绝；
3. 解析导出`nlang_native_init`；没有该导出的模块被拒绝；
4. 调用它一次（§27.3）。

任何一步的拒绝都在init运行之前卸载模块：不兼容模块的注册——指向宿主即将立刻卸载的库的指针——绝不可进入原生表。被拒绝的模块在调用处抛出宿主级执行期错误（§8.1）。

**生命周期。**已加载的模块保持加载直到宿主关闭，且模块按加载顺序的逆序卸载。

## 27.3 导出与注册

**导出。**模块的ABI界面是两个C链接符号，在ABI支持的每个平台上都不做名称改编：

| 导出 | 签名 | 契约 |
|---|---|---|
| `nlang_abi_version` | `unsigned (void)` | 返回`11`；唯一的版本权威（§27.2） |
| `nlang_native_init` | `int (void* registry, RegisterNativeFn reg, RegisterNativeBlockingFn regBlocking)` | 探针通过后调用一次 |

连同注册回调

```c
void reg        (void* registry, const char* ns,
                 const char* name, NativeFn fn);
void regBlocking(void* registry, const char* ns,
                 const char* name, NativeFn fn);
```

**注册。**在`nlang_native_init`内部，模块经`reg`注册它的每个函数——非阻塞原生，在不持任何锁的情况下运行（§10.1）——或经`regBlocking`——带阻塞段的原生（§27.7）——并把不透明的`registry`指针原样传递过去。注册的键是`<ns>.<name>`。`nlang_native_init`的返回值仅供参考；探针才是唯一的版本权威。

**头文件。**整个接口——版本常量、函数类型、宿主表与导出宏——由公共头文件`nlang/vm/NativeHost.h`定义。模块包含它，并用所提供的宏书写其init体：

```c
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "math", "sqrt", &mathSqrt);
    regBlocking(registry, "fs", "readFile", &fsReadFile);
}
```

该宏自动发出`nlang_abi_version`探针，所以模块不可能偏离头文件的版本常量。

## 27.4 调用约定与值单元

**签名。**原生函数的签名为

```c
void fn(NativeHost* host, uint8_t* ret,
        const uint8_t* args, int argc);
```

- `host`——宿主函数表（§27.5）；
- `ret`——接收返回值的一个值单元；`void`原生什么都不写；
- `args`——容纳各实参的`argc`个值单元的基址；
- `argc`——`args`中值单元的数目：自由函数为声明的形参数，方法或构造器多一个，把接收者计入（见下文「实参」）。

**实参。**自由原生函数在`args[0]`收到它的第一个形参。方法或构造器在`args[0]`收到接收者，其余形参排在其之后（§7.5）。默认实参在分派之前于调用点填充，所以`argc`总等于声明的数目（§7.5）。原生函数没有`out`参数（§7.5）。

**值单元。**每个实参、接收者与返回值都经由一个八字节单元传递：

| 内容 | 编码 |
|---|---|
| `int`、`short`、`byte`、`bool`、`char`及其无符号形式 | 值在低四字节 |
| `float` | IEEE 754 binary32位模式在低四字节 |
| `long`、`double` | 值或位模式在全部八字节 |
| `string`、类或结构体实例、数组、`List`、`Dict`、函数值 | 低四字节中是一个32位堆句柄 |

携带四字节值或句柄的单元，其另外四字节绝不被读取。句柄就是垃圾回收器追踪的那个标识符（§9.2）：原生函数经宿主表把它传回去，绝不直接解引用它。

**帧纪律。**原生函数只读`args`、只写`ret`，不做其他事；单元之外的帧内存对它不可见，且经宿主表取得的任何东西都不在调用之后存活（§27.5、§27.6）。

## 27.5 宿主函数表

**布局。**`NativeHost`是一个函数指针结构体，其第一个字段是`abiVersion`——一个持有`11`的`uint32_t`——之后按固定顺序跟随下列二十二个回调。每个回调都以表自身作为第一个参数，在签名中记作`self`：

| 回调 | 签名 |
|---|---|
| `getString` | `const char* (NativeHost* self, int32_t handle)` |
| `newString` | `int32_t (NativeHost* self, const char* utf8)` |
| `newListString` | `int32_t (NativeHost* self, const char* const* items, int count)` |
| `writeOutput` | `void (NativeHost* self, const char* text)` |
| `writeError` | `void (NativeHost* self, const char* text)` |
| `readLine` | `const char* (NativeHost* self)` |
| `readToken` | `const char* (NativeHost* self)` |
| `readChar` | `int (NativeHost* self, uint32_t* outChar)` |
| `hasInput` | `int (NativeHost* self)` |
| `raiseException` | `void (NativeHost* self, int exceptionKind, const char* message)` |
| `nextRandom` | `uint32_t (NativeHost* self)` |
| `seedRandom` | `void (NativeHost* self, int32_t seed)` |
| `getArrayView` | `int (NativeHost* self, int32_t handle, NativeArrayView* out)` |
| `newArray` | `int32_t (NativeHost* self, uint8_t elemKind, int32_t length)` |
| `newArrayShaped` | `int32_t (NativeHost* self, uint8_t elemKind, int32_t rank, const int32_t* dims)` |
| `newArrayView` | `int32_t (NativeHost* self, int32_t targetHandle, int32_t rank, const int32_t* dims, const int32_t* strides, int64_t offset)` |
| `nowMs` | `long long (NativeHost* self)` |
| `parkForIo` | `int (NativeHost* self, int fd, unsigned events, long long deadlineMs, int hasDeadline)` |
| `moduleState` | `void* (NativeHost* self, const char* tag, void* (*ctor)(void), void (*dtor)(void*))` |
| `wakeIoWaiters` | `void (NativeHost* self, int fd)` |
| `currentContextId` | `int (NativeHost* self)` |
| `submitWork` | `int (NativeHost* self, void (*fn)(void*), void* arg, int ackFd, int priority)` |

**字符串。**`getString`返回`handle`背后字符串的字节；`newString`从一个以NUL结尾的Unicode转换格式（UTF-8）缓冲区创建字符串并返回其句柄；`newListString`在一次调用内从`count`个以NUL结尾的缓冲区创建一个`List<string>`。`getString`返回的缓冲区只有效到下一次宿主回调——原生函数立即复制它。声明返回`string`的原生函数必须向`ret`写入一个由`newString`获得的句柄：句柄0表示原生函数没有产出字符串，会抛出带函数名的宿主级错误；不再活动的句柄同样抛出。空字符串是`newString`发出的真实句柄，绝不是0。

**控制台。**`writeOutput`把文本逐字写入标准输出（§14.2）；`writeError`把它写入诊断通道——标准错误，或嵌入宿主掌管各流时会话的单一合并输出视图（§14.2）。`readLine`返回剥去行尾换行符的一行输入，输入结束时返回空指针；空行是`""`（§14.3）。`readToken`跨行跳过空白并返回下一个记号；输入结束时宿主在返回之前抛出，所以空指针结果仅为防御性（§14.3）。`readChar`把一个完整的UTF-8标量解码进`outChar`并返回`0`；输入结束时宿主在返回之前抛出，所以`1`结果仅为防御性（§14.3）。不是有效UTF-8的文本抛出基类`Exception`（§14.3）。`hasInput`在`readLine`仍能产出一行时返回`1`，且从不抛出（§14.3）。输入回调返回的缓冲区只有效到下一次宿主回调。

**异常。**`raiseException`抛出由`exceptionKind`选中的那个类的异常，且永不返回：

| `exceptionKind` | 抛出的类 |
|---|---|
| `0` | `Exception` |
| `1` | `IOException` |
| `2` | `NullPointerException` |
| `3` | `IndexOutOfBoundsException` |
| `4` | `DivByZeroException` |

被抛出的对象是一条以`message`为其消息的§8.3常规异常：NLang代码捕获它与捕获一次`throw`完全相同（§8.1）。

**随机。**`nextRandom`抽取下一个值，`seedRandom`为生成器播种；状态属于宿主，并被本次运行中的每个原生函数共享。

**数组。**`newArray`铸造一个一维数组，`newArrayShaped`铸造一个给定秩的数组（§4.10），两者都零初始化，并返回句柄。元素种类是六个码之一，与§25.5的类型种类码一致：

| 码 | 元素类型 |
|---|---|
| `0` | `int` |
| `1` | `float` |
| `10` | `byte` |
| `11` | `ubyte` |
| `15` | `long` |
| `17` | `double` |

这六个之外的种类抛出基类`Exception`。负的长度或维度，以及超过分配守卫的任何请求，抛出`IndexOutOfBoundsException`——与语言侧负尺寸创建的结果相同（§4.10、§8.3）。`1`到`64`之外的秩抛出基类`Exception`（§4.10）。铸造出的句柄由宿主保持存活，直到原生函数把它写进`ret`（§27.7）。

`newArrayView`在`targetHandle`背后的数组——或视图——之上铸造一个视图（§4.10），采用给定的秩、各维长度、步幅与偏移（以元素计，§27.6）；元素种类取自目标。其校验就是语言自身视图创建所施加的那套：为null或非数组的目标分别抛出`NullPointerException`或`IndexOutOfBoundsException`，`1`到`64`之外的秩或非标量的元素种类抛出基类`Exception`，非法的维度或超出目标边界的范围抛出`IndexOutOfBoundsException`（§4.10）。铸造出的句柄由宿主保持存活，直到原生函数把它写进`ret`（§27.7）。

`getArrayView`用`handle`背后数组——持有记录或视图记录——的§27.6固定视图填好`*out`并返回`1`；为null或越界的句柄抛出`NullPointerException`，非数组值的句柄抛出基类`Exception`——不存在不抛出的失败路径。

**字节单元。**`byte`与`ubyte`数组采用窄单元约定：每个元素占一个`int32_t`单元，值存于其低字节。这类数组的视图与`int`数组一样报`elemCellBytes`为`4`；§27.6的寻址不变。

**IO停泊与工作池。**`nowMs`返回单调毫秒数——本节所有停泊截止时间共用的时钟域（§10.1）；它不是墙上时间。`parkForIo`为`fd`登记`events`——`NLANG_EV_READ`为`1`、`NLANG_EV_WRITE`为`2`——的感兴趣事件，外加可选的绝对截止时间，并按§27.7所述展开调用方。`wakeIoWaiters`注销轮询器对`fd`的感兴趣事件并唤醒停泊其上的每个上下文——套接字关闭路径；它不取自己的锁，可在不持锁时调用。`currentContextId`返回调用上下文的标识，在停泊与重入之间保持稳定（§10.1）——原生函数用它区分自己的重入与别的上下文的并发调用。它只能在阻塞段原生中调用。`submitWork`在宿主的通用原生工作池上运行`fn(arg)`，池中工作线程不持任何锁；完成时池向`ackFd`——调用方拥有的文件描述符（`-1`表示不需要）——写入一个字节。`priority`为`0`是普通、`1`是高优先——两级队列，慢速查找不会饿死短任务。池惰性启动，并在调度器关停时并入；`submitWork`本身经任一通道都可调用，返回`0`。

**模块状态。**`moduleState`返回以`tag`命名的、宿主管理的按执行器状态槽：宿主对每个执行器调用一次`ctor`，把结果与`dtor`一并存储，此后每次调用都交回同一指针；`dtor`在执行器关停时、模块本将卸载之前运行（模块从不卸载）。以这种方式存储的状态对另一个执行器永不可见——`net`包的套接字表正是依赖这一隔离。

## 27.6 固定数组视图

**视图。**`NativeArrayView`描述一个堆数组的载荷——持有记录的载荷，或视图记录在其持有记录中的窗口：

| 字段 | 类型 | 内容 |
|---|---|---|
| `rank` | `int32_t` | 数组的秩（§4.10） |
| `dims` | `const int32_t*` | 各维长度；指向数组记录内部 |
| `strides` | `const int32_t*` | 以元素计的步幅；指向视图自身内部 |
| `elemCount` | `int64_t` | 元素总数 |
| `elemKind` | `uint8_t` | 元素种类（§27.5） |
| `elemCellBytes` | `uint8_t` | 每元素步幅的字节数：`4`，`long`与`double`为`8` |
| `reserved` | `uint8_t[2]` | 写`0`；读取器忽略该字段 |
| `offset` | `int64_t` | 零号元素所在的位置，以元素计（§27.6寻址） |
| `stridesInline` | `int32_t[64]` | `strides`所指向的存储：64个条目，即秩上限（§27.5） |
| `data` | `void*` | 持有记录的载荷基址 |

**寻址。**下标为`i[0] … i[rank-1]`的元素位于

```
data + (offset + i[0]*strides[0] + … + i[rank-1]*strides[rank-1])
     * elemCellBytes
```

`offset`以元素计，与`strides`同一单位。持有数组报`0`和由自身各维长度算出的行主序步幅；视图报它记录的偏移与步幅，指向持有载荷，`data`仍指向持有记录的载荷基址。

**生命周期。**垃圾回收器不移动对象，且参数记录在原生调用期间保持存活，所以视图的每个指针在调用期间都稳定——且仅在调用期间。绝不要把视图缓存到返回之后。

**自有步幅。**`strides`指向视图自身内部，且每个视图拥有自己的步幅：复制该结构体会让副本的`strides`仍指向原视图。应当重新请求一个新视图，而不是复制一个。

## 27.7 阻塞段

**两条通道。**经`reg`注册的原生函数在不持任何锁的情况下运行：它不得等待虚拟机之外的任何东西，其存储操作经自守护回调逐次取得类别锁（§10.1）。经`regBlocking`注册的原生函数声明一个阻塞段：它等待外部世界可能花费无界时间，且虚拟机在其调用周围释放它持有的锁，让其他上下文在此期间运行（§10.1）。

**暂存。**阻塞段对签名是透明的。释放这些锁之前，宿主把参数单元与结果单元复制到C++栈存储上，使它们保持被回收器可达，并停泊调用上下文；原生返回后，它把结果复制回去并恢复上下文。原生函数在两种情形下看到的都是同样的`args`与`ret`。

**停泊。**将要等待文件描述符的阻塞原生不阻塞其载体，而是停泊：它以描述符、所等待的事件位和可选的绝对截止时间（`nowMs`域）调用`parkForIo`（§27.5）。首次进入时`parkForIo`返回`0`——感兴趣事件已登记，原生必须立即返回、不再触碰其他任何东西；宿主停泊调用上下文并释放载体。当描述符就绪或截止时间到，宿主以同样的暂存实参重新执行同一次原生调用，`parkForIo`此时返回`1`——原生重试该操作，要么完成、要么再次停泊。小于零的描述符仅按截止时间停泊。由于唤醒可能在所等待的条件并不成立时到来——别的上下文的关闭、一次陈旧唤醒——原生在每次返回时重新检查自己的条件，把`parkForIo`当作重试提示而非保证。停泊只对由语言分派的原生存在：宿主重新执行的是调用该原生的调用指令，所以由宿主自身发起的原生调用——原生main根窗口、宿主桥——没有这样的锚点，不得停泊。

**回调。**每个宿主回调都自我守护——它取得自己的存储所需的锁并在返回之前释放——所以阻塞原生可以在其段内任意一点调用任何回调；原生自身不得假设在段内独占虚拟机。

**扎根。**在阻塞段内铸造的句柄（§27.5）由宿主保持存活，直到原生函数把它写进`ret`；从未发布的铸造句柄在段结束后即可能被回收。

**传播。**在阻塞段内抛出的异常——经`raiseException`或经宿主回调——被搁置直到调用上下文恢复，然后像任何其他抛出一样传播到调用方的处理器（§8.1）。
