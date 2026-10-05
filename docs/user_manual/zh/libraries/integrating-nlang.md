# 集成NLang

把NLang嵌入自己的程序，当前的官方集成面是子进程驱动：你的程序调用`ncc`编译、调用`nvm`执行，通过标准输入输出与脚本交互。本页面向集成方：要带哪些文件、怎么部署、怎么驱动、怎么调试。

一个最小的目标程序长这样（本页反复用到它）：

```nlang
import io;

int main()
{
    io.print("hello from nlang");
    return 0;
}
```

## 1. 集成形态

| 形态 | 状态 | 说明 |
|---|---|---|
| 子进程驱动 | 现支持 | 宿主程序起`ncc`/`nvm`子进程，以退出码与标准流通信 |
| 进程内宿主应用程序编程接口（API，application programming interface） | 当前不提供 | 以C/C++库形式链接VM |

## 2. 依赖清单

以[安装与布局](../getting-started/install-layout.md)的安装目录为基准：

| 位置 | 内容 | 谁需要 |
|---|---|---|
| `bin\nvm.exe` | VM执行器 | 运行脚本 |
| `bin\ncc.exe` | 编译器 | 从源码编译 |
| `bin\ndb.exe` | 调试器 | 调试集成 |
| `bin\ndisasm.exe` | 字节码反汇编 | 深度诊断 |
| `bin\nide.exe`与Qt运行时 | 集成开发环境（IDE，integrated development environment）及其依赖 | 仅图形界面开发 |
| `bin\nlang_{io,math,fs}.dll` | 标准库的native实现 | 程序用到对应命名空间 |
| `stdlib\` | 标准库 | io/math/fs任意一项 |
| `docs\site\` | 手册站点 | 可选 |

只做运行不做编译时，`ncc`可以不带；Qt运行时只有`nide`需要。

## 3. 最小依赖集

最小集是`nvm.exe`＋`stdlib\stdlib.npkg`＋`nlang_io.dll`，加上程序自身的`.ncu`。把`nlang_io.dll`放在`nvm.exe`旁，`stdlib.npkg`放进`stdlib\`子目录，`nvm <程序>.ncu`即可运行。`stdlib\`里的`.n`声明源只在`ncc`编译期用到，运行期装载的只有库包；程序用到哪个命名空间，就带上对应的`nlang_*.dll`。可执行文件动态链接MSVC运行库，目标机器需具备VC++ 2015-2022运行库（见[安装与布局](../getting-started/install-layout.md)）。

## 4. 部署配方

- 推荐布局：应用目录旁放一个完整的NLang安装目录，用绝对路径或`NLANG_PATH`环境变量指过去。
- 项目构建把`.npkg`写在工程文件旁：不要把用户工程放进`Program Files`这类只读位置，`.npkg`会写不进去。
- 库查找排障：`--verbose`（或`-v`）按层打印解析后的导入搜索路径，见[ncc](../cli-tools/ncc.md)、[nvm](../cli-tools/nvm.md)；`-I`临时加目录。

## 5. 子进程驱动细节

- 退出码：程序`main`的返回值按32位原样透传，宿主直接以子进程退出码判定结果，约定见[退出码约定](../language-spec/exit-code-convention.md)。
- 标准输入：向`nvm`的stdin写行即可驱动`io.readLine`交互；程序输出按Unicode转换格式（UTF-8，Unicode Transformation Format）字节写入stdout，`io.eprint`走stderr。
- 编译与运行可以合并为一次`ncc <file>`调用，也可以分开`ncc build`＋`nvm`。

## 6. 调试集成

无人值守调试用`ndb --machine`行协议：协议事件（含`error`错误事件）走stdout，stderr只承载崩溃报告与`--verbose`搜索路径列表，宿主逐行解析即可驱动断点与单步。协议细节见[调试](../getting-started/debugging.md)与[ndb](../cli-tools/ndb.md)。

## 7. 展望

以C/C++库形式在进程内链接VM属于后续开发内容；在那之前，集成一律走子进程驱动。
