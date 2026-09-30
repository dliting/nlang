# 包

**包**是 NLang 的代码身份单位。每个 `.n` 文件就是一个包，包名只有
一个来源：文件相对其匹配搜索根的路径。文件内没有任何为包命名的
语法——旧版本携带的包装关键字已删除。

```nlang
// stdlib/io.n            -> 包 "io"
// <根>/vendor/graphics.n（配 -I <根>）-> 包 "vendor.graphics"
// <根>/gfx/color/deep.n  （配 -I <根>）-> 包 "gfx.color.deep"
import io;
import vendor.graphics;
import gfx.color.deep;
```

## 限定名

包声明的一切都通过限定名访问——`包.成员`（点分包则是
`包.子包.成员`）。导入只开放限定名：`import lib;` 之后写 `lib.f()`，
裸名 `f()` 不会经由导入解析。（与调用方同目录的文件共享该目录的
裸名池——同目录规则不变。）

类型同样带限定：`alib.Point`、`gfx.color.deep.Shade`。末段相同的两个
包（`a.io` 与 `b.io`）是两个不同的包，各自的函数与类型互不相串；
同一次构建内每个点分包名只能存在一份——两个单元解析出同一包名是
编译错误，诊断指名两条来源路径。

## 编译模块与限定表键

编译模块（`.nmod`，格式 v1.13）里每个 struct/class/函数表键都**带
包名限定**：`main.main`、`utils.helper.help`、`alib.Point`。无属主的
内建保持裸键（`Object`、`List`）。限定键同时是调试器与工具的统一
拼写：

- 断点写限定名：`b main.main`、`b mathutil.triple`；
- 回溯与停止行打印限定名：`#0 main.main (main.n:6)`、
  `Stopped: utils.helper.inner`；
- `ndisasm -func main.main` 按同一拼写过滤；
- 函数类型的值按其键渲染：`func alib.twice`；对象的默认
  `toString()` 渲染限定类键（`alib.Point@1a2b`）。

## 流与字面量

`readStruct`/`readObject` 以字符串取类型名。该字面量在**编译期**
按本文件可见的包（自身包＋已导入的包）定身，并改写为声明的限定
表键，到达 VM 的永远不会是裸名。两个可见包声明同名类型时字面量
有歧义——编译错误要求写限定形（`readStruct("alib.S")`）。对象流
同样存限定类键：一个程序写出、另一个程序读入的对象必须来自相同
的包布局，因为读取端按键直接查表。

## native 声明

`native` 声明的宿主 DLL 按包的**首段**命名
（`nlang_<段>.dll`），因此多段包（`gfx.color.deep`）里的 `native`
无法命名宿主库，会在编译期得到诊断。单段包里的 `native` 声明
行为不变。

## 限定类型上的泛型

内建泛型容器支持类型实参（`List<int>`、`Dict<string, int>`）。
带类型实参的限定**用户**类型（`alib.Box<int>`）目前不是语言的一
部分：解析器以 syntax error 拒绝该形状。（指名这一限制的专用
诊断计划在后续阶段补上。）
