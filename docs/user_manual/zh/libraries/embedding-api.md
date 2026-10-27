# 嵌入NLang

除了[集成NLang](integrating-nlang.md)所述的子进程驱动，NLang还提供进程内宿主应用程序编程接口（API，application programming interface）：宿主C++程序链接`nlang_embed`库，在自己的进程里装载并执行编译产物，与脚本交换类型化的值。本页覆盖最小宿主、生命周期、宿主函数、值交换、错误处理与输入输出重定向；完整可运行示例见安装目录`examples\embed_host\`。子进程方式的入门概览见[将NLang嵌入到其他程序中](../getting-started/embedding-nlang.md)。

## 1. 依赖与构建

- 头文件：`include\nlang\embed\NLang.h`（链接时向编译器加安装目录的`include\`）。
- 库：`nlang_embed`（静态库，连带`nlang_vm`、`nlang_runtime`与编译器前端）。
- 脚本用到标准库的native命名空间时，相应`nlang_*.dll`既可放宿主可执行文件旁，也可放`addImportDir`给出的导入目录（搜索顺序见第3节）。

以CMake为例：

```cmake
add_executable(my_host main.cpp)
target_link_libraries(my_host PRIVATE nlang_embed)
```

## 2. 最小宿主

```cpp
#include "nlang/embed/NLang.h"
#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: my_host <program.ncu|.npkg>\n");
        return 2;
    }
    nlang::initialize();
    nlang::Interpreter itp;
    try {
        itp.load(argv[1]);
        return itp.run();
    } catch (const nlang::Exception& e) {
        std::fprintf(stderr, "script error: %s\n", e.message().c_str());
        return 1;
    } catch (const nlang::LoadError& e) {
        std::fprintf(stderr, "load error: %s\n", e.what());
        return 1;
    }
}
```

先用`ncc`把脚本编译成`.ncu`（或打进`.npkg`），宿主再装载运行；`run()`返回脚本`main`的退出码。

## 3. 从源码执行（ad-hoc脚本）

除装载编译产物外，解释器还可以直接执行源码：`runSource`接收源码字符串，`runFile`接收脚本文件路径，都在进程内完成编译和执行并返回退出码（顶层`return`的值即退出码；无`return`时为0）。脚本免写`main()`样板——顶层语句自动包裹进合成入口，与`nlang`命令行工具同一语义。

```cpp
nlang::Interpreter itp;
itp.addImportDir("path/to/stdlib");   //进程内嵌入不自动发现标准库
itp.setOutputHandler(
    [](const char* s) { std::printf("%s", s); }, nullptr);
int rc = itp.runSource("import io; io.print(6*7);");   //rc == 0
```

要`import io`，先把标准库目录加入导入路径——库声明与包的解析走`addImportDir`给出的目录，`runFile`形态下脚本所在目录还会自动加入搜索（进程内嵌入不读`NLANG_PATH`，也不搜索宿主可执行文件目录或当前目录）；原生模块（`nlang_io.dll`等）的解析同样走导入目录，但可执行文件所在目录恒居原生搜索表首位——残留该处的旧版DLL会遮蔽导入目录里的副本。典型部署是把`stdlib`目录——`.n`声明、`stdlib.npkg`、原生动态链接库（DLL，dynamic-link library）——整个放到应用数据目录，启动时`addImportDir`一次。

执行完成后仍可`call`脚本声明的函数（键为`<模块名>.<函数名>`）：字符串源码的模块名固定为`script`；`runFile`的模块名是文件名词干，词干须为合法标识符——`my-script.n`这类含连字符的文件名会被拒绝。

```cpp
nlang::Interpreter itp;   //每个实例只能定义一次程序来源
(void)itp.runSource("int getTimeout() { return 30; } return 0;");
nlang::Value t = itp.call("script.getTimeout", {});   //30
```

每个`Interpreter`的程序来源只能定义一次（`load`/`runSource`/`runFile`三选一）；编译失败不消耗这次机会，可以修正源码后重试。再跑一段脚本就再建一个`Interpreter`——实例之间不共享状态。宿主函数注册与输出重定向须在`runSource`/`runFile`之前完成。

只需跑一段代码时，还有一对便捷函数：

```cpp
nlang::ScriptOptions opts;
opts.importDirs.push_back("path/to/stdlib");
int rc = nlang::runScript("import io; io.print(\"hi\");", opts);
```

`runScript`/`runScriptFile`内部新建解释器、执行、销毁，不保留状态；需要宿主函数或跑后`call`时请用完整的`Interpreter`流程。顶层`return`建议取0-255：进程退出码按32位原值传递，POSIX shell只观察低8位（见[退出码约定](../language-spec/exit-code-convention.md)）。

## 4. 生命周期

| 调用 | 次数与时机 | 说明 |
|---|---|---|
| `initialize()` | 每进程至少一次，幂等 | 初始化运行时；`shutdown()`经`atexit`自动注册，宿主通常不必显式调用 |
| `addImportDir(d)` | 任意次，均须在装载程序来源之前 | 追加`import`的链接期搜索目录（如`stdlib`所在目录） |
| `load(artifact)` | 每实例恰一次，与`runSource()`/`runFile()`三选一 | 装载`.ncu`/`.npkg`并装齐其全部依赖；失败抛`LoadError` |
| `runSource(src)`/`runFile(path)` | 每实例恰一次，与`load()`三选一 | 进程内编译 + 执行源码脚本（见第3节）；失败抛`LoadError`，编译失败不消耗程序来源 |
| `run()` | 至多一次 | 调脚本`main`，返回退出码；未捕获的脚本异常抛`Exception` |
| `call(name, args)` | 任意次（装载后、`run()`前后皆可） | 按「单元词干.函数名」键调用模块级函数 |

同一个`Interpreter`实例内不得重入：宿主函数回调里再调同一实例的`run()`/`call()`会抛`BadValue`。

### 并发与挂起

`run()`经调度器的载体（carrier，承载虚拟线程运行的操作系统线程）执行：脚本里的`thread.sleep`按截止时刻唤醒、`thread.spawn`派生的上下文得到调度、`thread.join`挂起等待——与`nvm`命令行运行完全一致。`main`挂起且再无可运行上下文时（例如空通道`recv`无发送者），`run()`抛可捕获的`std::runtime_error`（消息含`deadlock`），不会挂死。

`run()`以单载体默认执行：`Interpreter`用一条载体驱动调度器（库默认值），任意时刻只有一个虚拟线程在解释字节码，上下文只在挂起点之间推进。存储操作按数据类别分组加锁（见[载体与并行](../language-spec/concurrency-carriers.md)）；在`VmExecutor`层嵌入的宿主可在`Execute`前调`SetCarrierThreadCount(n)`显式选入多载体——纯计算段与对象字段访问跨载体并行，按ABI（应用二进制接口，application binary interface）第5版声明为阻塞的原生调用挂起当前上下文并放锁，其窗口内其余载体执行其他虚拟线程。宿主回调因此可能与其他载体上的字节码以及彼此之间并发运行，共享状态须自行同步。同一执行器仍同时至多运行一次`Execute`；多个宿主线程并发`CallByName`时各自取调度器锁，因而依次执行。同一个`Interpreter`实例一次只由一个宿主线程驱动——它的重入守卫是误用检查，不是并发原语。

经`registerHostFunction`注册的宿主函数一律走阻塞原生协议：VM暂存其实参，挂起当前上下文并放锁，回调执行期间不占用载体，返回后取锁续行。暂存的实参与返回值在该窗口内登记为垃圾回收（GC，garbage collection）根，其他载体上的回收不会回收在途句柄；此类调用内发起的NativeHost回调每次重新取锁。

`call()`是宿主发起的重入求值，没有自己的载体轮转：

- 会挂起的内在函数一律拒绝，抛`std::runtime_error`（消息含`cannot suspend`）：`thread.yield`、`thread.sleep`、目标未完成的`thread.join`、目标未完成的`Future.get`、竞争中的`Mutex.lock`、空通道`Channel.recv`、满通道`Channel.send`。
- `thread.spawn`同样拒绝（消息含`spawn`）——新上下文无载体可调度；需要并发的宿主流程请经`run()`进入。
- 不挂起的形态照常可用：`Mutex.tryLock`、`Future.isDone`、未满通道的`Channel.send`、缓冲非空的`recv`、目标已完成的`join`、目标已完成的`Future.get`。

## 5. 宿主函数

脚本侧以`native`声明外部函数，宿主侧注册实现：

```cpp
itp.registerHostFunction("host", "now",
    [](const std::vector<nlang::Value>& args) {
        return nlang::Value(int32_t(42));
    });
```

```nlang
//文件名必须是host.n——声明所在单元的词干就是键的前半段
native int now();

int main() {
    return 100 - now();   //58
}
```

键规则：`registerHostFunction`的命名空间参数须等于声明文件名词干（上例`host`）。回调收到的实参按脚本侧声明的类型编组（声明`string`收到`Kind::String`）；回调抛出的C++异常会变成脚本里可`catch`的`Exception`，脚本抛出的`Exception`经回调重抛时保持原实例。

## 6. 值交换

`nlang::Value`按`kind()`分档：`Null`、`Int`、`Long`、`Float`、`Double`、`Bool`、`Char`、`String`与引用类`Array`、`List`、`Dict`、`Object`、`Struct`、`Func`。

- 标量：`asInt()`/`asString()`等访问器，kind不匹配抛`BadValue`。窄整型（byte/short等）折叠到`Int`/`Long`，枚举折叠到`Int`。
- 字符串：值语义——两侧各自持有拷贝，改动互不可见。
- 引用类（容器与对象）：`asList()`/`asDict()`/`asArray()`/`asObject()`返回读写代理，宿主的改动对脚本立即可见，反之亦然。容器元素与对象字段按脚本侧声明类型校验，错配抛`BadValue`。
- 宿主构造容器：`newList()`/`newDict()`返回builder值，元素暂存在宿主侧；每次作为实参跨入脚本都会创建一个新容器实例（无缓存），元素按形参声明的元素类型校验。
- 内存安全：宿主持有的引用值登记为垃圾回收（GC，garbage collection）根，脚本触发回收不会回收宿主还在用的对象。代理与引用值的有效期受所属`Interpreter`约束。

```cpp
nlang::Value v = itp.call("mymod.makeList", {});
nlang::ListProxy list = v.asList();
list.set(0, nlang::Value(int32_t(9)));       //脚本侧立即可见
nlang::Value sum = itp.call("mymod.sumList", {v});
```

## 7. 错误处理

| 异常类 | 含义 | 捕获建议 |
|---|---|---|
| `nlang::Exception` | 未捕获的脚本异常（脚本侧错误） | 宿主边界捕获，读`message()`/`backtrace()`/`exceptionClass()` |
| `nlang::BadValue` | 宿主用法错误：kind错配、索引越界、重入、生命周期误用 | 属宿主缺陷，修宿主代码而非捕获绕过 |
| `nlang::LoadError` | 装载与环境失败：坏路径、坏产物、native模块缺失 | 捕获后向使用者报告环境问题 |
| `std::runtime_error` | VM内部错误与并发边界：死锁报告、重入求值内的挂起或`thread.spawn`拒绝 | 宿主边界捕获并报告 |

## 8. 输入输出重定向

`setOutputHandler(out, err)`把脚本的`io.print`与`io.eprint`分别转发到两个回调（文本按Unicode转换格式（UTF-8，Unicode Transformation Format）字节到达）；两个回调都为空即卸装，回到真实的标准流。回调在执行线程上触发，运行期间不持全局锁——并发输出经控制台互斥量串行，保持短小可减少排队；回调不得抛异常。输入侧默认无通道：脚本首次`io.readLine`抛出可捕获的`IOException`；宿主注入输入属于后续开发内容。

```cpp
itp.setOutputHandler(
    [](const char* text) { std::fprintf(stdout, "[nlang] %s", text); },
    [](const char* text) { std::fprintf(stderr, "[nlang-err] %s", text); });
```
