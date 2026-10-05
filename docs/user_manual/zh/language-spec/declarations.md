# 声明


类型声明——`enum`、`struct`、`class`、`interface`——各有独立页面：[枚举](enum.md)、[结构体](struct.md)、[类](class.md)、[接口](interface.md)。本页覆盖其余声明：import、变量、类型别名。

### import声明

```nlang
import io;                 // 标准库包
import lib;                // 外部模块 lib.ncu / lib.npkg
import utils.helper;       // 项目文件 utils/helper.n
import utils.*;            // 递归通配符
```

`import`声明**本文件**可以引用哪些模块——导入集属于所在翻译单元，从不会泄漏给其他文件。三种来源共用一套语法：

| 来源 | 模块路径 | 示例 |
|---|---|---|
| 项目文件 | 相对`.nproj`根的点分路径：目录路径 + 文件名主干 | `utils/helper.n` → `utils.helper`；根目录`main.n` → `main` |
| 外部模块 | 文件名主干（单段）：`.ncu`文件，或恰好暴露一个模块的`.npkg` | `lib.ncu` / `lib.npkg` → `lib` |
| 标准库包 | `io` / `math` / `fs`（随工具链分发的库源） | `io` |

可见性：

| 引用 | 需要导入？ | 调用形式 |
|---|---|---|
| 同一文件 | 否 | 裸名 |
| 同目录其他项目文件 | 否（隐式） | 裸名**或**限定名 |
| 跨目录、同一项目 | **是**（`import utils.helper;`或`import utils.*;`） | 仅限定名：`utils.helper.f()` |
| 外部模块 | **是**（`import lib;`） | 仅限定名：`lib.f()` |
| 标准库`io`/`math`/`fs` | **是**（`import io;`） | 限定名：`io.print` |

- 裸名解析只覆盖本文件与同目录文件；其余一律按模块路径限定。无属主符号（`Exception`类家族等根级内建、native宿主绑定）保持全局裸名可见。
- 通配符`import utils.*;`是模块路径空间中的**递归前缀匹配**：以`utils.`开头的每条路径都可导入（`utils.helper`、`utils.sub.x`、……）。它只缩写导入清单——调用时仍写完整路径。通配符只匹配项目文件；外部模块名是单段的，永不匹配。
- `import utils;`只匹配根文件`utils.n`；要访问`utils/`目录请用完整路径或通配符。
- 重复导入是幂等的；精确导入与通配符重叠时取并集；导入自身模块路径或同目录文件是无害的冗余。
- 导入目标的解析顺序：编译进本次构建的模块（项目文件，或搜索根上的库源——标准库与第三方源码库同一机制）→ 外部模块（`.ncu`/`.npkg`，经`-I`）。没有隐式回退。
- 同一次构建内两个单元解析出同一个点分包名是编译错误，诊断会指名两条来源路径。名为`io`/`math`/`fs`的项目目录就是普通目录；每个包名只能存在一份。带点导入会按匹配到的搜索根解析库源（`-I <根>`＋`<根>/a/b/c.n`即可寻址`import a.b.c;`）；预编译的带点包目前仍按末段主干搜索。
- 外部模块的代码不进产物：消费方映像只记录导入槽，运行期由加载器沿同一搜索路径重新定位它（机制见[命令行工具/ncc](../cli-tools/ncc.md)的「产物与加载期链接」）。

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
Point pt;                   // struct（零初始化）
Node n = new Node();        // class（堆上分配）
Node n2;                    // class（null）
```

struct变量零初始化（所有字段 = 0）。class变量默认为null。类型特定的声明形式及其语义在各类型页：[枚举](enum.md)、[结构体](struct.md)、[类](class.md)、[接口](interface.md)。

### 类型别名

`using Name = Type;`声明**类型别名**——任何类型表达式的简写，凡是期望类型的位置都可用：

```nlang
using Grid = Dict<string, List<int[]>>;
using Ints = int[];
using BinOp = Func<int, int>;

Grid g;                  // 与完整类型完全等价
List<Grid> lg;           // 泛型实参内亦然
int apply(BinOp f) { ... }   // 参数与返回类型
```

**语义：**
- 别名在**名称解析前的前置趟**展开：每个使用点都被替换为被别名类型的深拷贝，因此行为与写出完整类型完全一致。
- 作用域是**翻译单元**——同一程序的不同模块里，同一别名可以映射到不同类型。
- 右侧可以**前向引用**文件中后声明的类型（与直接写出该类型相同）。
- 别名可以引用**另一个别名**，但后者必须在文件中**更早**声明（文本顺序）；前向的别名引用是编译错误。
- 别名不得与同一翻译单元内的class、函数、其他别名、内建类型名（`List`、`Dict`、`Func`、`int`、……）或已索引的库包名（标准库`math`/`io`/`fs`，以及搜索路径上发现的第三方库包）撞名。
- 开命名空间的`using Foo;`形式（无`=`）保持不变，与此无关。

**限制：**
- 右侧必须是**基本类型名**（`int`、`float`、`string`等）、**数组类型**（`int[]`）、**泛型实例化**（`List<int>`）或**函数类型**（`Func<int, int>`）。裸的**class/struct/enum类型名**（`using X = Counter;`）不是合法目标——会以解析器语法错误失败。**成员路径**（`using X = ns.Inner;`）同样不支持——文法的类型形式只接受标识符，因此会以解析器语法错误失败。
- 别名只能出现在类型位置；值表达式永远不会解析到别名。
- 锚定在使用点的诊断有时会指向`using`行（展开克隆优先采用别名目标的位置）。
