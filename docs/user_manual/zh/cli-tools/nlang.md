# nlang —— 直接运行脚本

nlang不经`main()`样板直接运行NLang语句，用法与python解释器一致：把一段顶层语句交给它执行。可以写一个脚本文件运行，也可以用`-c`把几条语句直接写在命令行里，适合一次性任务、教学示例与快速试验。nlang在进程内完成编译与执行，不在磁盘留下任何产物。

## 调用形态

| 形态 | 命令 | 行为 |
|---|---|---|
| 运行脚本文件 | `nlang <script.n> [-I <dir>...]` | 编译并在进程内执行 |
| 运行内联语句 | `nlang -c "<语句>" [-I <dir>...]` | 语句写入临时`.n`后按脚本执行 |
| 版本查询 | `nlang --version` | 输出`nlang (NLang) <版本>` |

每次调用只做一件事：要么给脚本文件，要么给`-c`。两样都给、都没给，或者出现不认识的标志时，nlang打印用法并以退出码1结束。`-c`的文本原样写进一个临时`.n`文件（内嵌换行保留），按脚本执行，诊断里的行号就来自这个文件。命令行参数按宽字符解码，中文字符串这类美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）之外的内容可以直接使用。

示例中的引号写法面向cmd与bash。Windows PowerShell 5.1里保留反斜杠，外层改用单引号：`nlang -c 'import io; io.print(\"hello\");'`。PowerShell 7里用单引号并去掉反斜杠：`nlang -c 'import io; io.print("hello");'`。或者把语句写进`.n`文件运行`nlang <文件>`，完全绕开引号问题。

## 标志

| 标志 | 说明 |
|---|---|
| `-c "<语句>"` | 内联语句——作为脚本执行，退出码即顶层return |
| `-I <dir>`（或`-I<dir>`连写） | 库搜索路径，可重复给出：编译期查找被import的`.n`与外部`.ncu`/`.npkg`，运行期定位全部依赖成员与native动态库 |
| `--version` | 版本查询，在任何初始化之前生效 |

搜索路径的层序与ncc相同（`-I`＞脚本所在目录＞`NLANG_PATH`＞标准库等系统目录）。标准库不需要`-I`或任何配置——`import io;`后即可用`io.print`；要引用第三方库时，用`-I`指路（见[ncc](ncc.md)页的同名条目）。

## 脚本模式语义

顶层语句被包裹进一个合成的`int main()`。这个合成入口与手写`main()`在产物与行为上没有区别，整条编译、链接、执行链路与`ncc <文件>`一致。合成只发生在有顶层语句的文件里：没有顶层语句的普通程序（手写`main()`，如`examples/`里的示例）用nlang运行，行为与`ncc <文件>`相同。具体语义：

- **退出码 = 顶层return**：`return 42;`让进程以42退出；没有顶层return时退出0。裸`return;`是错误（缺少返回值）。
- **空脚本退出0**：没有可执行语句的文件（空文件、只有声明或只有import）不会合成入口，nlang按成功处理，退出0。这与nvm不同：nvm运行无入口的产物时会退出1。
- **声明与语句混排**：函数、class、struct、enum、interface可与顶层语句任意交错；全部声明在装载期注册（书写位置无关），语句按源文顺序执行。`import`与`using`仍须写在文件头。顶层语句与手写`main()`写进同一文件会被重名检查拒绝。
- **被import的单元不执行语句**：库单元只装载声明。含顶层语句的库在编译期就会报错（`contains top-level statements`）。这一点与python不同：python的模块体在import时会执行。
- **顶层break/continue被拒绝**：顶层`break`要求外围有循环或switch，`continue`要求外围有循环，否则是编译错误。
- **未捕获异常**：stderr打印`Runtime error: ...`与调用回溯，退出1。

## func前缀前置声明

顶层的裸`int f();`是语法错误，因为空参数形式必须属于函数定义。只声明不定义时，用`func`前缀显式标记：

```nlang
func int twice(int v);   //声明：实现在别处（native或外部链接）

int x = 21;              //本文件不调用twice
return x;
```

`func`形态表示函数在别处实现（native注册或外部链接）——独立脚本提供不了这种实现，所以脚本里只声明、不调用。调用同文件里靠后定义的函数根本不需要声明，`func`只用于实现在文件之外的情况。修饰符引导的声明不受影响，`native int probe();`照常。声明与同文件定义并存时，重复定义检查会拒绝。函数体内的`Foo f();`是无参直接初始化——声明变量并调用其无参构造函数——照常可用；空参歧义只出现在顶层（见[已知限制](../language-spec/known-limitations.md)）。

## 范围之外

nlang面向ad-hoc运行：没有交互式REPL、不接收脚本参数、不产出`.ncu`/`.npkg`、不支持`.nproj`项目。需要这些能力时用ncc与nvm（见[概览](overview.md)）。
