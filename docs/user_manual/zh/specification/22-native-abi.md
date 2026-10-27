> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 22. 原生应用二进制接口（ABI）

*原生模块*是一个共享库，它依据原生应用二进制接口（ABI）实现原生函数——§7.5中声明为`native`、经限定键`<ns>.<name>`调用的那些函数。标准库模块与第三方模块使用同一界面。§22.1界定范围并固定ABI版本；§22.2固定模块发现与加载；§22.3固定模块导出与注册；§22.4固定调用约定与值单元；§22.5定义宿主函数表；§22.6定义固定数组视图；§22.7陈述阻塞段契约。

## 22.1 范围与版本

**ABI版本。**本章描述ABI版本 **9**。宿主接受版本探针恰好报`9`的模块，并拒绝其他一切值，包括根本不导出探针的模块（§22.2）。ABI变更会提升版本并整章替换本章；规范只描述当前版本。

**本章固定的内容。**模块发现与加载（§22.2）、模块导出与注册（§22.3）、调用约定与值单元（§22.4）、宿主函数表（§22.5）、固定数组视图（§22.6）与阻塞段契约（§22.7）。

**本章不固定的内容。**语言侧的`native`声明、它的按键方式与实参规则（§7.5）；单元映像内部的指令编码（第20章）；以及宿主不经模块、直接提供原生实现所用的嵌入接口——这样的宿主实现在任何模块查找之前应答，受调用约定与宿主函数表约束，但不受模块导出约束。

**无状态性。**原生模块不保持任何语言可观察的状态：伪随机数生成器（PRNG）与控制台住在宿主里，只能经宿主函数表到达（§22.5）。

## 22.2 模块发现与加载

**模块文件。**实现命名空间`<ns>`的模块是一个共享库，在Windows上名为`nlang_<ns>.dll`，在macOS上名为`libnlang_<ns>.dylib`，在其他POSIX系统上名为`libnlang_<ns>.so`。

**惰性加载。**在键`<ns>.<name>`尚未注册的首次调用上，宿主载入该命名空间的模块——命名空间就是第一个点号之前的段（§7.5）。不带点号的键是裸名，绝不触发模块加载。每个命名空间只加载一次；同一命名空间中所有后续调用复用已加载的模块。

**搜索顺序。**宿主在一个有序目录列表中搜索模块文件。正在运行的可执行文件所在目录总在列表中；嵌入宿主可以添加目录，开发构建会追加存放其自身原生产物的构建树目录。在任何目录都找不到的模块，在调用处抛出宿主级执行期错误（§9.1），并命名该文件与每个被查询过的目录。

**加载序列。**宿主打开模块文件，然后：

1. 解析导出`nlang_abi_version`；没有该导出的模块被拒绝；
2. 调用它；除`9`以外的值被拒绝；
3. 解析导出`nlang_native_init`；没有该导出的模块被拒绝；
4. 调用它一次（§22.3）。

任何一步的拒绝都在init运行之前卸载模块：不兼容模块的注册——指向宿主即将立刻卸载的库的指针——绝不可进入原生表。被拒绝的模块在调用处抛出宿主级执行期错误（§9.1）。

**生命周期。**已加载的模块保持加载直到宿主关闭，且模块按加载顺序的逆序卸载。

## 22.3 导出与注册

**导出。**模块的ABI界面是两个C链接符号，在ABI支持的每个平台上都不做名称改编：

| 导出 | 签名 | 契约 |
|---|---|---|
| `nlang_abi_version` | `unsigned (void)` | 返回`9`；唯一的版本权威（§22.2） |
| `nlang_native_init` | `int (void* registry, RegisterNativeFn reg, RegisterNativeBlockingFn regBlocking)` | 探针通过后调用一次 |

连同注册回调

```c
void reg        (void* registry, const char* ns,
                 const char* name, NativeFn fn);
void regBlocking(void* registry, const char* ns,
                 const char* name, NativeFn fn);
```

**注册。**在`nlang_native_init`内部，模块经`reg`注册它的每个函数——非阻塞原生，在全局锁持有期间被调用（§11.1）——或经`regBlocking`——带阻塞段的原生（§22.7）——并把不透明的`registry`指针原样传递过去。注册的键是`<ns>.<name>`。`nlang_native_init`的返回值仅供参考；探针才是唯一的版本权威。

**头文件。**整个接口——版本常量、函数类型、宿主表与导出宏——由公共头文件`nlang/vm/NativeHost.h`定义。模块包含它，并用所提供的宏书写其init体：

```c
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "math", "sqrt", &mathSqrt);
    regBlocking(registry, "fs", "readFile", &fsReadFile);
}
```

该宏自动发出`nlang_abi_version`探针，所以模块不可能偏离头文件的版本常量。

## 22.4 调用约定与值单元

**签名。**原生函数的签名为

```c
void fn(NativeHost* host, uint8_t* ret,
        const uint8_t* args, int argc);
```

- `host`——宿主函数表（§22.5）；
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

携带四字节值或句柄的单元，其另外四字节绝不被读取。句柄就是垃圾回收器追踪的那个标识符（§10.2）：原生函数经宿主表把它传回去，绝不直接解引用它。

**帧纪律。**原生函数只读`args`、只写`ret`，不做其他事；单元之外的帧内存对它不可见，且经宿主表取得的任何东西都不在调用之后存活（§22.5、§22.6）。

## 22.5 宿主函数表

**布局。**`NativeHost`是一个函数指针结构体，其第一个字段是`abiVersion`——一个持有`9`的`uint32_t`——之后按固定顺序跟随下列十五个回调。每个回调都以表自身作为第一个参数，在签名中记作`self`：

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

**字符串。**`getString`返回`handle`背后字符串的字节；`newString`从一个以NUL结尾的Unicode转换格式（UTF-8）缓冲区创建字符串并返回其句柄；`newListString`在一次调用内从`count`个以NUL结尾的缓冲区创建一个`List<string>`。`getString`返回的缓冲区只有效到下一次宿主回调——原生函数立即复制它。

**控制台。**`writeOutput`把文本逐字写入标准输出（§15.2）；`writeError`把它写入诊断通道——标准错误，或嵌入宿主掌管各流时会话的单一合并输出视图（§15.2）。`readLine`返回剥去行尾换行符的一行输入，输入结束时返回空指针；空行是`""`（§15.3）。`readToken`跨行跳过空白并返回下一个记号；输入结束时宿主在返回之前抛出，所以空指针结果仅为防御性（§15.3）。`readChar`把一个完整的UTF-8标量解码进`outChar`并返回`0`；输入结束时宿主在返回之前抛出，所以`1`结果仅为防御性（§15.3）。不是有效UTF-8的文本抛出基类`Exception`（§15.3）。`hasInput`在`readLine`仍能产出一行时返回`1`，且从不抛出（§15.3）。输入回调返回的缓冲区只有效到下一次宿主回调。

**异常。**`raiseException`抛出由`exceptionKind`选中的那个类的异常，且永不返回：

| `exceptionKind` | 抛出的类 |
|---|---|
| `0` | `Exception` |
| `1` | `IOException` |
| `2` | `NullPointerException` |
| `3` | `IndexOutOfBoundsException` |
| `4` | `DivByZeroException` |

被抛出的对象是一条以`message`为其消息的§9.3常规异常：NLang代码捕获它与捕获一次`throw`完全相同（§9.1）。

**随机。**`nextRandom`抽取下一个值，`seedRandom`为生成器播种；状态属于宿主，并被本次运行中的每个原生函数共享。

**数组。**`newArray`铸造一个一维数组，`newArrayShaped`铸造一个给定秩的数组（§4.3），两者都零初始化，并返回句柄。元素种类是四个码之一，与§20.5的类型种类码一致：

| 码 | 元素类型 |
|---|---|
| `0` | `int` |
| `1` | `float` |
| `15` | `long` |
| `17` | `double` |

这四个之外的种类抛出基类`Exception`。负的长度或维度，以及超过分配守卫的任何请求，抛出`IndexOutOfBoundsException`——与语言侧负尺寸创建的结果相同（§4.3、§9.3）。`1`到`64`之外的秩抛出基类`Exception`（§4.3）。铸造出的句柄由宿主保持存活，直到原生函数把它写进`ret`（§22.7）。

`getArrayView`用`handle`背后数组的§22.6固定视图填好`*out`并返回`1`；为null或越界的句柄抛出`NullPointerException`，非数组值的句柄抛出基类`Exception`——不存在不抛出的失败路径。

## 22.6 固定数组视图

**视图。**`NativeArrayView`描述一个堆数组的载荷：

| 字段 | 类型 | 内容 |
|---|---|---|
| `rank` | `int32_t` | 数组的秩（§4.3） |
| `dims` | `const int32_t*` | 各维长度；指向数组记录内部 |
| `strides` | `const int32_t*` | 以元素计的行主序步幅；指向视图自身内部 |
| `elemCount` | `int64_t` | 元素总数 |
| `elemKind` | `uint8_t` | 元素种类（§22.5） |
| `elemCellBytes` | `uint8_t` | 每元素步幅的字节数：`4`，`long`与`double`为`8` |
| `reserved` | `uint8_t[2]` | 写`0`；读取器忽略该字段 |
| `stridesInline` | `int32_t[64]` | `strides`所指向的存储：64个条目，即秩上限（§22.5） |
| `data` | `void*` | 载荷基址——零号元素 |

**寻址。**下标为`i[0] … i[rank-1]`的元素位于

```
data + (i[0]*strides[0] + … + i[rank-1]*strides[rank-1])
     * elemCellBytes
```

**生命周期。**垃圾回收器不移动对象，且参数记录在原生调用期间保持存活，所以视图的每个指针在调用期间都稳定——且仅在调用期间。绝不要把视图缓存到返回之后。

**自有步幅。**`strides`指向视图自身内部，且每个视图拥有自己的步幅：复制该结构体会让副本的`strides`仍指向原视图。应当重新请求一个新视图，而不是复制一个。

## 22.7 阻塞段

**两条通道。**经`reg`注册的原生函数在全局锁持有期间运行：它不得等待虚拟机之外的任何东西（§11.1）。经`regBlocking`注册的原生函数声明一个阻塞段：它等待外部世界可能花费无界时间，且虚拟机在其调用周围释放全局锁，让其他上下文在此期间运行（§11.1）。

**暂存。**阻塞段对签名是透明的。释放锁之前，宿主把参数单元与结果单元复制到C++栈存储上，使它们保持被回收器可达，并停泊调用上下文；原生返回后，它把结果复制回去并恢复上下文。原生函数在两种情形下看到的都是同样的`args`与`ret`。

**回调。**每个宿主回调都在内部重新取得全局锁，所以阻塞原生可以在其段内任意一点调用任何回调；原生自身不得假设在段内独占虚拟机。

**扎根。**在阻塞段内铸造的句柄（§22.5）由宿主保持存活，直到原生函数把它写进`ret`；从未发布的铸造句柄在段结束后即可能被回收。

**传播。**在阻塞段内抛出的异常——经`raiseException`或经宿主回调——被搁置直到调用上下文恢复，然后像任何其他抛出一样传播到调用方的处理器（§9.1）。
