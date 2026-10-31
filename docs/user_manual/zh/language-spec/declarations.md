# 声明

规范性规则见规范§3.3–§3.5。

类型声明——`enum`、`struct`、`class`、`interface`——各有独立页面：[枚举](enum.md)、[结构体](struct.md)、[类](class.md)、[接口](interface.md)。本页覆盖其余声明：import、变量、类型别名。

### import声明

```nlang
import io;                 // 标准库包
import lib;                // 外部模块lib.ncu / lib.npkg
import utils.helper;       // 项目文件utils/helper.n
import utils.*;            // 递归通配符
```

`import`声明**本文件**可以引用哪些模块——导入集属于所在翻译单元，从不会泄漏给其他文件。三种来源共用一套语法：

| 来源 | 模块路径 | 示例 |
|---|---|---|
| 项目文件 | 相对`.nproj`根的点分路径：目录路径 + 文件名主干 | `utils/helper.n` → `utils.helper`；根目录`main.n` → `main` |
| 外部模块 | 文件名主干（单段）：`.ncu`文件，或恰好暴露一个模块的`.npkg` | `lib.ncu` / `lib.npkg` → `lib` |
| 标准库包 | `io` / `math` / `fs` / `thread` / `concurrent`（随工具链分发的库源） | `io` |

可见性：

| 引用 | 需要导入？ | 调用形式 |
|---|---|---|
| 同一文件 | 否 | 裸名 |
| 同目录其他项目文件 | 否（隐式） | 裸名**或**限定名 |
| 跨目录、同一项目 | **是**（`import utils.helper;`或`import utils.*;`） | 仅限定名：`utils.helper.f()` |
| 外部模块 | **是**（`import lib;`） | 仅限定名：`lib.f()` |
| 标准库`io`/`math`/`fs`/`thread`/`concurrent` | **是**（`import io;`） | 限定名：`io.print` |

- 裸名解析只覆盖本文件与同目录文件；其余一律按模块路径限定。无属主符号（`Exception`类家族等根级内建、native宿主绑定）保持全局裸名可见。
- 通配符`import utils.*;`是模块路径空间中的**递归前缀匹配**：以`utils.`开头的每条路径都可导入（`utils.helper`、`utils.sub.x`、……）。它只缩写导入清单——调用时仍写完整路径。通配符只匹配项目文件；外部模块名是单段的，永不匹配。
- `import utils;`只匹配根文件`utils.n`；要访问`utils/`目录请用完整路径或通配符。
- 重复导入是幂等的；精确导入与通配符重叠时取并集；导入自身模块路径或同目录文件是无害的冗余。
- 导入目标的解析顺序：编译进本次构建的模块（项目文件，或搜索根上的库源——标准库与第三方源码库同一机制）→ 外部模块（`.ncu`/`.npkg`，经`-I`）。没有隐式回退。
- 同一次构建内两个单元解析出同一个点分包名是编译错误，诊断会指名两条来源路径。名为`io`/`math`/`fs`的项目目录就是普通目录；每个包名只能存在一份。带点导入会按匹配到的搜索根解析库源（`-I <根>` + `<根>/a/b/c.n`即可寻址`import a.b.c;`）；预编译的带点包目前仍按末段主干搜索。
- 外部模块的代码不进产物：消费方映像只记录导入引用，运行期由加载器沿同一搜索路径重新定位它（机制见[命令行工具/ncc](../cli-tools/ncc.md)的「产物与加载期链接」）。

诊断信息（示例）：

```
Module 'utils.helper' is not imported. Add 'import utils.helper;' (or 'import utils.*;') at the top of this file.
Package 'io' is not imported. Add 'import io;' at the top of this file.
Module 'utils.helper' not found. Check the project Sources list or -I import path.
String import is removed. Use 'import <module>;' with an identifier path.
Function 'add' is not visible here. It lives in module 'utils.helper'; import it and qualify the call.
```

### 变量声明

```nlang
int x = 5;
float y = 3.14;
string s = "hello";
Color c = Color.Red;       // enum
Point pt;                   // struct（字段默认值，或0）
Node n = Node(7);        // class（堆上分配）
Node n2;                    // class（null）
```

struct变量取字段默认值——未声明初始化器时所有字段 = 0，声明了则为所声明的默认值。class变量默认为null。

`T name();`是**无参构造**：执行类型的无参构造函数，顶层与函数体内一致（`Foo f();`等价于`Foo f = Foo();`）。基本类型用此形态报编译错误（`"Int32" is not a class or struct type.`）。无函数体的函数声明写法保留`func`标记（`func Foo f();`）。

一条声明可以用逗号列出多个声明符，且每个声明符都可以使用首声明符的任意形态——裸声明、`= 值`、`= {…}`初始化器、`(参数)`直接初始化、`()`零参构造、`[n]`定长、`[n] = [...]`定长带条目：

```nlang
int a, b = 5;               // a为0，b为5
int xs[3], ys[2];           // 两个长度不同的数组
Vec v(2), w();              // 都执行构造（w走零参构造函数）
List<int> l, m = {1, 2};    // m获得花括号初始化器
int p[3], q;                // p是int数组，q是普通int
```

每个声明符的类型是语句头类型加上它自己的括号：`int a[3], b;`中的`b`是普通`int`，不是数组。头类型本身是数组时（`int[] a, b;`），全部声明符都是数组。

任何声明引入的名字——变量、参数、字段、循环变量——都不能复用可见的类型名：`class Counter`可见时，`int Counter = 5;`报`"Counter" names a type; it cannot be used as a variable name.`

### 默认值

声明不带初始化时，各类型的默认值如下（不产生任何编译告警）：

| 类型 | 默认值 |
|---|---|
| 8个整型（`byte`、`ubyte`、`short`、`ushort`、`int`、`uint`、`long`、`ulong`） | 0 |
| `float`、`double` | 0 |
| `bool` | false |
| `char` | 码位0 |
| `string` | null句柄，读作空字符串 |
| enum | 底层int值0 |
| struct | 每个字段的默认值（未声明初始化器时0） |
| class、interface、`Object` | null |
| 数组、`List`、`Dict` | null |
| `func` | null |

引用类型的默认值是null：`== null`可以直接判断；不判空就使用——调用方法、取成员、取下标、取长度——会在运行期抛出`null reference error`并附回溯。string是唯一的例外：null句柄读作空字符串，`.length()`得0、拼接时如同`""`（见[字符串](string.md)）。enum的默认值是底层int值0：成员自动递增（从0起）时恰好是首个成员——`Color c;`之后`c == Color.Red`成立；显式赋非零值的enum，默认0可能不落在任何成员上。

类型特定的声明形式及其语义在各类型页：[枚举](enum.md)、[结构体](struct.md)、[类](class.md)、[接口](interface.md)。

### 类型别名

`using Name = Type;`声明**类型别名**——任何类型表达式的简写，凡是期望类型的位置都可用：

```nlang
using Grid = Dict<string, List<int[]>>;
using Ints = int[];
using BinOp = func<int, int>;

Grid g;                  // 与完整类型完全等价
List<Grid> lg;           // 泛型实参内亦然
int apply(BinOp f) { ... }   // 参数与返回类型
```

**语义：**
- 别名在**名称解析前**统一展开：每个使用点都被替换为它指向的类型，因此行为与写出完整类型完全一致。
- 作用域是**翻译单元**——同一程序的不同模块里，同一别名可以映射到不同类型。
- 右侧可以**前向引用**文件中后声明的类型（与直接写出该类型相同）。
- 别名可以引用**另一个别名**，但后者必须在文件中**更早**声明（文本顺序）；前向的别名引用是编译错误。
- 别名不得与同一翻译单元内的class、函数、其他别名、内建类型名（`List`、`Dict`、`func`、`int`、……）、预置包声明的类名（`Object`、异常家族、`String`、`Array`、……）或已索引的库包名（标准库`math`/`io`/`fs`/`thread`/`concurrent`，以及搜索路径上发现的第三方库包）撞名。
- 打开作用域的`using Foo;`形式（无`=`）是另一条指令：它必须指明一个包，而当前实现下这样的指令一律被拒绝（规范§3.4）。

**限制：**
- 右侧必须是一个**类型**：**基本类型名**（`int`、`float`、`string`等）、**class/struct/enum类型名**（`using X = Counter;`）、**数组类型**（`int[]`）、**泛型实例化**（`List<int>`）或**函数类型**（`func<int, int>`）。**成员路径**（`using X = ns.Inner;`）不支持——声明本身能通过语法检查，但每个使用点都无法解析出类型。
- 别名只能出现在类型位置；值表达式永远不会解析到别名。
- 锚定在使用点的诊断有时会指向`using`行（展开克隆优先采用别名目标的位置）。

### 预置包`builtins`与同名优先级

每次构建自动导入预置包`builtins`（`stdlib/builtins.n`，随工具链发布）。包内把基础容器`List<T>`与`Dict<K,V>`、异常家族、层次根`Object`以及声明持有类`String`与`Array`声明为普通class——构造器元数与方法面就是文件里声明的样子。见[标准库](standard-library.md)。

自己的代码在根级别声明同名class会**遮蔽**预置声明——任何位置都是最内层声明获胜（类型引用、构造调用、成员查找）：

```nlang
class Exception { }          // 合法：遮蔽预置的Exception

Exception e = Exception();   // 解析到你自己的class
```

同名规则是统一的：

- **类型位置与构造调用**都绑定到最内层声明。
- **变量命名**：变量不能取任何在该处可见的类型名。即使有上面的遮蔽，`int Exception = 1;`仍是编译错误——该名字仍然解析到一个类型。
- **类型别名**不能取预置包声明的名字（`using Exception = int;`是编译错误），无论是否遮蔽。

自己包内的声明（包`alib`里的`alib.Object`）不遮蔽任何东西——只有根级别声明才遮蔽；限定名`alib.Object`与预置类两者互不干扰。
