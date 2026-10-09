# 嵌入NLang

除了[集成NLang](integrating-nlang.md)所述的子进程驱动，NLang还提供进程内宿主应用程序编程接口（API，application programming interface）：宿主C++程序链接`nlang_embed`库，在自己的进程里装载并执行编译产物，与脚本交换类型化的值。本页覆盖最小宿主、生命周期、宿主函数、值交换、错误处理与输入输出重定向；完整可运行示例见安装目录`examples\embed_host\`。

## 1. 依赖与构建

- 头文件：`include\nlang\embed\NLang.h`（链接时向编译器加安装目录的`include\`）。
- 库：`nlang_embed`（静态库，连带`nlang_vm`、`nlang_runtime`与编译器前端）。
- 脚本用到标准库的native命名空间时，相应`nlang_*.dll`须在宿主可执行文件旁（装载子系统的搜索规则与`nvm`相同，见[集成NLang](integrating-nlang.md)）。

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

## 3. 生命周期

| 调用 | 次数与时机 | 说明 |
|---|---|---|
| `initialize()` | 每进程至少一次，幂等 | 初始化运行时；`shutdown()`经`atexit`自动注册，宿主通常不必显式调用 |
| `addImportDir(d)` | 任意次，均须在`load()`之前 | 追加`import`的链接期搜索目录（如`stdlib`所在目录） |
| `load(artifact)` | 每实例恰一次 | 装载`.ncu`/`.npkg`并解析闭包；失败抛`LoadError` |
| `run()` | 至多一次 | 调脚本`main`，返回退出码；未捕获的脚本异常抛`Exception` |
| `call(name, args)` | 任意次（`load()`后、`run()`前后皆可） | 按「单元词干.函数名」键调用模块级函数 |

同一个`Interpreter`实例内不得重入：宿主函数回调里再调同一实例的`run()`/`call()`会抛`BadValue`。

## 4. 宿主函数

脚本侧以`native`声明外部函数，宿主侧注册实现：

```cpp
itp.registerHostFunction("host", "now",
    [](const std::vector<nlang::Value>& args) {
        return nlang::Value(int32_t(42));
    });
```

```nlang
//文件名必须是 host.n——声明所在单元的词干就是键的前半段
native int now();

int main() {
    return 100 - now();   //58
}
```

键规则：`registerHostFunction`的命名空间参数须等于声明文件名词干（上例`host`）。回调收到的实参按脚本侧声明的类型编组（声明`string`收到`Kind::String`）；回调抛出的C++异常会变成脚本里可`catch`的`Exception`，脚本抛出的`Exception`经回调重抛时保持原实例。

## 5. 值交换

`nlang::Value`按`kind()`分档：`Null`、`Int`、`Long`、`Float`、`Double`、`Bool`、`Char`、`String`与引用类`Array`、`List`、`Dict`、`Object`、`Struct`、`Func`。

- 标量：`asInt()`/`asString()`等访问器，kind不匹配抛`BadValue`。窄整型（byte/short等）折叠到`Int`/`Long`，枚举折叠到`Int`。
- 字符串：值语义——两侧各自持有拷贝，改动互不可见。
- 引用类（容器与对象）：`asList()`/`asDict()`/`asArray()`/`asObject()`返回读写代理，宿主的改动对脚本立即可见，反之亦然。容器元素与对象字段按脚本侧声明类型校验，错配抛`BadValue`。
- 宿主构造容器：`newList()`/`newDict()`返回builder值，元素暂存在宿主侧；每次作为实参跨入脚本都物化成一个新容器实例（无缓存），元素按形参声明的元素类型校验。
- 内存安全：宿主持有的引用值登记为垃圾回收（GC，garbage collection）根，脚本触发回收不会回收宿主还在用的对象。代理与引用值的有效期受所属`Interpreter`约束。

```cpp
nlang::Value v = itp.call("mymod.makeList", {});
nlang::ListProxy list = v.asList();
list.set(0, nlang::Value(int32_t(9)));       //脚本侧立即可见
nlang::Value sum = itp.call("mymod.sumList", {v});
```

## 6. 错误处理

| 异常类 | 含义 | 捕获建议 |
|---|---|---|
| `nlang::Exception` | 未捕获的脚本异常（脚本侧错误） | 宿主边界捕获，读`message()`/`backtrace()`/`exceptionClass()` |
| `nlang::BadValue` | 宿主用法错误：kind错配、索引越界、重入、生命周期误用 | 属宿主缺陷，修宿主代码而非捕获绕过 |
| `nlang::LoadError` | 装载与环境失败：坏路径、坏产物、native模块缺失 | 捕获后向使用者报告环境问题 |

## 7. 输入输出重定向

`setOutputHandler(out, err)`把脚本的`io.print`与`io.eprint`分别转发到两个回调（文本按Unicode转换格式（UTF-8，Unicode Transformation Format）字节到达）；两个回调都为空即卸装，回到真实的标准流。回调在执行线程上触发，不得抛异常。输入侧默认无通道：脚本首次`io.readLine`抛出可捕获的`IOException`；宿主注入输入属于后续开发内容。

```cpp
itp.setOutputHandler(
    [](const char* text) { std::fprintf(stdout, "[nlang] %s", text); },
    [](const char* text) { std::fprintf(stderr, "[nlang-err] %s", text); });
```
