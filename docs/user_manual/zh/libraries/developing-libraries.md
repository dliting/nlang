# 开发第三方库

NLang的库机制只有一套：一个库就是导入搜索路径上的一个目录，标准库与第三方库走完全相同的发现、编译与执行路径。本页面向库作者，讲三件事：纯NLang库怎么组织、带native实现的混合库怎么写、写好之后怎么分发。

机制内部的设计依据见[库机制](../vm-architecture/library-mechanism.md)，import语法与可见性规则见[包与模块](../language-spec/packages.md)。本页示例与仓库中的`examples/libdemo/`完全一致，可直接编译运行。

## 1. 库的形态

导入搜索路径上的一个`.n`文件就是一个包，包名取文件名去掉扩展名；搜索目录的子目录构成点分包名。例如搜索目录`libs`下：

```text
libs/
  mylib/
    math.n        -> 包 mylib.math
    text.n        -> 包 mylib.text
  quick.n         -> 包 quick
```

消费方`import mylib.math;`之后用限定名调用，如`mylib.math.square(6)`。裸名（不带包前缀）访问不到导入的成员——依赖在调用点一目了然是刻意的设计。

搜索路径的五个层次、`-I`与`NLANG_PATH`的用法见[ncc](../cli-tools/ncc.md)与[nvm](../cli-tools/nvm.md)。点分名字的库不单独编译成单元——它们用工程文件打包分发，见第4节。

## 2. 纯NLang库

完整示例即`examples/libdemo/`（平面形态：库文件与使用方同目录）：

```text
libdemo/
  main.n          消费方程序
  mylib.n         库：两个数学小工具
```

`mylib.n`的全部内容：

```text
// Library example: two math helpers forming their own compilation unit.
// A library has no entry point, so compile it with the build form first:
//   ncc build examples/libdemo/mylib.n -o mylib.ncu
int square(int x) {
    return x * x;
}

int sum(int[] values) {
    int total = 0;
    foreach (int v in values) {
        total += v;
    }
    return total;
}
```

`main.n`的全部内容：

```text
// Consumer example: imports mylib (same directory) and verifies it.
// After building mylib.ncu, compile and run in one step:
//   ncc examples/libdemo/main.n -I <dir holding mylib.ncu> -o main_run.ncu
// Exit 0 only when the assertions hold.
import io;
import mylib;

int main() {
    assert(mylib.square(6) == 36);
    int[] values = [1, 2, 3, 4];
    assert(mylib.sum(values) == 10);
    io.print("mylib ready: " + mylib.sum(values));
    return 0;
}
```

NLang的产物按编译单元划分：编译`main.n`只生成消费方自己的单元，库代码不在其中。库单元单独编译一次，之后编译并运行使用方：

```text
> ncc build mylib.n -o mylib.ncu
> ncc build main.n -o main.ncu
> nvm main.ncu
mylib ready: 10
```

`nvm`运行`main.ncu`时，装载器发现它导入`mylib`，沿搜索路径找到`mylib.ncu`，两个单元在加载期完成链接——库不必进任何全局注册表，摆在搜索路径上即可。（库没有`main`，`ncc mylib.n`的编译＋执行形态会报module has no entry point；库一律用`build`形态编译。）

断言失败会抛异常并以非零码退出，所以示例自带正确性检查。

## 3. 混合库：NLang与native实现

需要操作系统应用程序编程接口（API，application programming interface）或现成C++代码时，在库里声明`native`函数并随包提供一个动态链接库（DLL，dynamic-link library）。声明侧（包`num`的`num.n`）：

```text
native int dbl(int x);

int quad(int x)
{
    return dbl(dbl(x));
}
```

实现侧（`num_native.cpp`）：

```cpp
#include <nlang/vm/NativeHost.h>

using namespace nlang::native;

static void Dbl(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc)
{
    ReturnInt(ret, ArgInt(args, 0) * 2);
}

NLANG_DEFINE_NATIVE_INIT
{
    reg(registry, "num", "dbl", &Dbl);
    return NLANG_HOST_ABI_VERSION;
}
```

宿主与库之间的应用二进制接口（ABI，application binary interface）契约：

| 项 | 约定 |
|---|---|
| DLL名 | `nlang_<命名空间>.dll`，如`nlang_num.dll` |
| 导出 | 仅`nlang_native_init`一个入口，由`NLANG_DEFINE_NATIVE_INIT`宏生成 |
| 注册 | `reg(registry, "命名空间", "函数名", &实现)`按名派发 |
| ABI版本 | 初始化函数返回`NLANG_HOST_ABI_VERSION`，与VM不匹配即拒载 |
| 头文件 | `include/nlang/vm/NativeHost.h`随包提供，不需要链接VM库 |
| 参数助手 | `ArgInt`/`ArgString`/`ReturnInt`/`ReturnString`等 |

用任意C++编译器把实现编成`nlang_num.dll`（头文件路径指向仓库`include/`），与`num.n`放在同一目录。分发时库单元与DLL一起交付（见下节）。`native`声明的类型规则见[native函数](../language-spec/native-functions.md)。

## 4. 分发

两种形态：

- 源码目录（平面）：把`mylib.n`放进任一搜索目录，消费方编译时内联解析签名；库单元要先编译一次（`ncc build mylib.n -o mylib.ncu`，见第2节），与使用方一起交付。最简单，也是标准库的形态。
- 预编译包：`ncc build -p <工程>.nproj -o <包名>.npkg`把库单元打包，适合不带源码分发；点分子目录库（如`mylib/math.n`）也以这种形态配套分发。库单元的映像随包走，链接在加载期完成——使用方`-I`指向包所在目录即可，全程不需要库源码。

使用方一侧无需区别对待：两种形态都按`import <包名>;`消费，搜索路径五层见[ncc](../cli-tools/ncc.md)。

## 5. 兼容性注意

宿主ABI版本不匹配时装载被拒绝（数字为双方各自报告的版本，随错配对象不同而不同）：

```text
Runtime error: native module '<DLL路径>\nlang_mylib.dll' reports incompatible ABI version 999999; host expects 3
Backtrace:
  at use_mylib.main (use_mylib.n:13)
```

升级NLang后重编native DLL即可对齐版本。除宿主ABI外，库与消费方各自编译，没有别的二进制耦合。
