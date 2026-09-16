# 声明


### import 声明

```nlang
import io;                 // 内建命名空间
import lib;                // 外部 lib.nmod
import utils.helper;       // 项目文件 utils/helper.n
import utils.*;            // 递归通配符
```

`import` 声明**本文件**可以引用哪些模块——导入集属于所在翻译单元，
从不会泄漏给其他文件。三种来源共用一套语法：

| 来源 | 模块路径 | 示例 |
|---|---|---|
| 项目文件 | 相对 `.nproj` 根的点分路径：目录路径 + 文件名主干 | `utils/helper.n` → `utils.helper`；根目录 `main.n` → `main` |
| 外部 `.nmod` | 文件名主干（单段） | `lib.nmod` → `lib` |
| 内建命名空间 | `io` / `math` / `fs`（保留名，预置模块） | `io` |

可见性：

| 引用 | 需要导入？ | 调用形式 |
|---|---|---|
| 同一文件 | 否 | 裸名 |
| 同目录其他项目文件 | 否（隐式） | 裸名**或**限定名 |
| 跨目录、同一项目 | **是**（`import utils.helper;` 或 `import utils.*;`） | 仅限定名：`utils.helper.f()` |
| 外部 `.nmod` | **是**（`import lib;`） | 仅限定名：`lib.f()` |
| 内建 `io`/`math`/`fs` | **是**（`import io;`） | 限定名：`io.print` |

- 裸名解析只覆盖本文件与同目录文件；其余一律按模块路径限定。无属主
  符号（`Exception` 类家族等根级内建、原生宿主绑定）保持全局裸名
  可见。
- 通配符 `import utils.*;` 是模块路径空间中的**递归前缀匹配**：以
  `utils.` 开头的每条路径都可导入（`utils.helper`、`utils.sub.x`、
  ……）。它只缩写导入清单——调用时仍写完整路径。通配符只匹配项目
  文件；外部 `.nmod` 名是单段的，永不匹配。
- `import utils;` 只匹配根文件 `utils.n`；要访问 `utils/` 目录请用
  完整路径或通配符。
- 重复导入是幂等的；精确导入与通配符重叠时取并集；导入自身模块
  路径或同目录文件是无害的冗余。
- 已知限制：跨目录共享的命名空间（两个文件声明同一个
  `namespace NS`）的成员在 v1 中从另一目录不可达——裸名调用被裸名
  池规则拒绝，又不存在限定形式（模块路径只寻址根级函数），因此
  「导入它并限定调用」提示给出的修复建议对它们无效。此限制随类型级
  可见性门收口。
- 导入目标的解析顺序：内建 → 项目文件 → 外部 `.nmod`（经 `-I`）。
  没有隐式回退。
- 项目路径段不得与 `io`/`math`/`fs` 撞名（编译错误）。单文件模式
  （无 `.nproj`）只支持单段导入——内建与外部 `.nmod`；点分路径无法
  解析。

诊断信息（示例）：

```
Module 'utils.helper' is not imported. Add 'import utils.helper;' (or 'import utils.*;') at the top of this file.
Namespace 'io' is not imported. Add 'import io;' at the top of this file.
Module 'utils.helper' not found. Check the project Sources list or -I import path.
String import is removed. Use 'import <module>;' with an identifier path.
Module path segment 'io' collides with a built-in namespace.
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

struct 变量零初始化（所有字段 = 0）。class 变量默认为 null。

### enum 声明

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

枚举值在运行期是 int32。成员可显式赋值，也可自动递增。

#### 枚举方法（Phase 12）

```nlang
enum Color {
    Red = 1, Green = 2, Blue = 4;

    public int weight(int base) {
        return this * base;
    }

    public int isPrimary() {
        switch (this) {
            case Color.Red, Color.Green, Color.Blue: return 1;
        }
        return 0;
    }
}
```

方法声明在成员列表之后，以 `;` 分隔。只有孤立结尾 `;` 而无方法的
`enum E { A; }` 也接受（Java 式收尾语法）。每个方法都必须有方法体
（抽象方法被拒绝）。

- **`this` 就是枚举值。**方法体内 `this` 是接收者的 int32 值：可直接
  参与算术与 `switch`（`this * base`、`switch (this)`），没有装箱。
  方法静态分派（`OP_CallMethodDirect`）——枚举没有继承，也没有虚
  分派。
- **经接收者调用。**`c.weight(3)`、`this.weight(3)`、
  `Color.Red.weight(3)`——任何枚举值表达式都可以。裸写的 `weight(3)`
  （无接收者）是编译错误，与 class 方法一致。
- **参数：**不支持默认值（`int f(int a, int b = 5)` 被拒绝），也不
  支持 `out` 参数。
- **`toString` 保留。**`toString()` 内建函数（值 → 名字）不能被
  用户方法遮蔽。
- **`this.<成员>` 按常量解析。**方法内的 `this.Red` 读的是成员
  `Red` 的值——不与接收者比较。方便，但容易误读；拿不准时请显式
  命名接收者。
- **成员/方法同名即冲突**（`enum E { f; int f() {...} }` 被拒绝）。
  访问修饰符遵循 class 方法规则。
- **不支持跨模块枚举。**被导入模块里声明的枚举类型对导入方不可见
  （`.nmod` 格式只序列化枚举名，不序列化声明）——这是模块格式的
  既有限制，与方法无关。
- 枚举数组：不能对数组本身调用方法——先索引出元素（`a[i].rank()`，
  不是 `a.rank()`）。

### struct 声明

```nlang
struct Point {
    int x;
    int y;
}
```

struct 可以包含：
- 基本类型字段（int、float、string）
- enum 字段（按 int32 存储）
- struct 字段（深拷贝，由外层 struct 持有）
- class 字段（引用，浅拷贝）

struct 不能包含方法。需要行为请用 class。

### class 声明

```nlang
class Node {
    public int value;
    public Node next;

    Node(int v) {
        this.value = v;
    }

    public int getValue() {
        return this.value;
    }
}

class SpecialNode : Node {
    public int extra;
}
```

class 支持：
- 带访问修饰符的字段（public/private/protected）
- 构造函数（可选，与类同名）
- 方法（隐式 `this` 参数）
- 单继承（`class Child : Parent`）
- 虚方法（`virtual` 关键字，动态分派）
- 子类方法覆写

**继承布局**：对象内存布局是
`[classIdx, 祖先字段..., 父类字段..., 自身字段...]`。
slot[0] 处的 `classIdx` 标识运行期类，供虚分派使用。

**构造函数行为**：只调用本类自己的构造函数；祖先构造函数不会被自动
调用。子类构造函数可用 `super(args);` 转发到直接父类的构造函数
（见下文「super()——构造函数链」）。没有显式 `super()` 时，从祖先
继承的字段零初始化。

**隐式 `this.field`（裸成员访问）**：在方法或构造函数内，解析到
外层类字段（含继承字段）的裸标识符是隐式 `this.field` 访问。读、
赋值、复合赋值（`v += 1`）与默认参数表达式
（`int add(int x, int y = v)`）都适用。同名局部变量或参数会遮蔽
字段，与 Java/C# 语义一致。

### interface 声明

```nlang
interface IShape {
    public int Area();
    public int Perimeter();
}

class Square : IShape {
    public int side;
    public int Area() { return this.side * this.side; }
    public int Perimeter() { return 4 * this.side; }
}

int TotalArea(IShape s) {
    return s.Area();   // 经接口虚分派
}
```

接口支持：
- 仅方法签名（无字段、无实现）
- `class X : IShape`（或 `class X implements IShape`）——类用 `:` 或
  `implements` 关键字声明符合接口
- 对接口类型的局部变量/参数/字段虚分派
- 多态集合（混合 `Square`/`Circle` 的 `List<IShape>`）

**接口声明中方法的 `public` 关键字必须写**。NLang 的默认访问修饰符
是 `private`；声明成 `int m();`（无 `public`）的接口方法能通过解析，
但按 private 对待，调用点**访问不到**。由此得到的错误消息——
"The function X does not exist or is not accessible"——有误导性：
方法存在，只是不公开。接口里请始终写 `public int m();`。（Java/C#
式的「接口成员天然公开」是未来的语言设计决策，不是当前行为。）

### 隐式 `Object` 基类

凡未显式继承其他类的 class 都隐式继承 `Object`。Object 由编译器
合成——没有源码级的 `class Object { ... }` 声明，用户也不写
`class Foo : Object`（该语法被拒绝）。

Object 提供两个带默认恒等语义的虚方法：

```nlang
int Equals(Object other);    // 恒等：同一堆引用 → 1，否则 0
int GetHashCode();           // 恒等：`this` 的堆索引（null 为 0）
```

`Equals` 与 `GetHashCode` 经按名分派实现虚方法——子类只需声明同名
方法即可覆写（不需要 `override` 关键字；运行期沿类层次上溯，最先
命中最派生的实现）：

```nlang
class Point {
    public int x;
    public int y;
    int GetHashCode() {              // 覆写 Object.GetHashCode
        return this.x * 31 + this.y;
    }
}
```

**string 的值语义**：string 虽是基本类型，但 `string.getHashCode()`
与 `string.equals(string)` 调用被内建化为*值*语义（哈希用
`std::hash`，Equals 用内容比较）。这使 string 无需包装类即可在后续
阶段用作 Dict 键。

**`==` 运算符不变**：Object.Equals 是可选实现（opt-in）的方法。class
引用上的 `==` 运算符继续直接比较堆索引（既有 `class_null` /
`class_virtual` 回归测试保持通过）。`Equals` 单独存在的原因，是允许用户
类以值相等覆写它，而不破坏更大代码库中恒等相等测试。

**装箱（基本类型 → Object）**：基本类型值（int / float / string）
赋给 Object 类型的目标时被隐式装箱：

```nlang
Object o = 5;            // int 装箱
Object f = 3.14;         // float 装箱
Object s = "hi";         // string 装箱

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 在调用点装箱
```

运行期表示是 kind 为 `RTK_Boxed` 的带标签槽位（slot[0] = 类型标签，
slot[1] = 值位）。装箱槽位不持有引用，GC MarkPhase 显式跳过它们。

**`null` 字面量的装箱保持**：字面量 `0`（用作 `null`）使 OP_Box
短路——不分配堆槽位，值 `0` 原样留在 Object 槽位内容里。这让
`Object o = null` 与 `Object o = 0` 都成为无操作，而不是把 0 包进
装箱 int 的堆引用。

**拆箱与 class 向下转换（`as` 运算符）**——Phase 8e-1.5：

```nlang
Object o = 5;
int x = o as int;          // 显式拆箱——装箱类型 ≠ int 时抛错

Object f = 3.14;
int bad = f as int;        // 抛错：期望 int，得到 float

class Point { public int x; }
Point p = new Point();
Object obj = p;
Point q = obj as Point;    // 显式 class 向下转换——运行期检查
Other o = obj as Other;    // 抛错：期望 Other，得到 Point
```

选择 `as` 关键字而非 C 风格的 `(T)expr` 前缀转换，是因为
`(T)expr` 会与带括号表达式产生 LALR(1) 冲突（解析器无法区分
`(foo) + bar` 与 `(foo + bar)`）。`as` 这类关键字运算符没有这种
歧义。C#、TypeScript 与 Kotlin 采取同一思路。

`as` 支持的转换：
- `TCK_Same`——无操作（如同一种基本类型或同一个 class）
- `TCK_Box`——基本类型到 Object（与隐式装箱路径对称）
- `TCK_Unbox`——Object 到基本类型（经 OP_Unbox 做运行期标签检查）
- `TCK_Downcast`——Object 到子类（经 OP_CheckCast 做运行期类检查，
  沿堆槽位的父类链上溯）

其他转换（如 `int as float`、`int as string`）是编译错误——请用
既有的基本类型强制转换 / `ToString()` 路径。

**Object 向上转换特例**：AST 层面的 `SnClassDecl::SuperClass()` 不
包含隐式 Object 父类（只有 VmBackend 的
`CompiledClass.superClassIdx` 包含）。转换检查器对
`target == Object`（任何 class 向上转换都是 TCK_Same，无操作）与
`source == Object`（任何 class 向下转换都是 TCK_Downcast）做了特例
处理，因此 `Object o = somePoint;` 与 `o as Point` 无需 Point 的 AST
父类链提及 Object 即可工作。

### 类型别名（Phase 13）

`using Name = Type;` 声明**类型别名**——任何类型表达式的简写，凡是
期望类型的位置都可用：

```nlang
using Grid = Dict<string, List<int[]>>;
using Ints = int[];
using BinOp = Func<int, int>;

Grid g;                  // 与完整类型完全等价
List<Grid> lg;           // 泛型实参内亦然
int apply(BinOp f) { ... }   // 参数与返回类型
```

**语义：**
- 别名在**名称解析前的前置趟**展开：每个使用点都被替换为被别名类型
  的深拷贝，因此行为与写出完整类型完全一致。
- 作用域是**翻译单元**——同一程序的不同模块里，同一别名可以映射到
  不同类型。
- 右侧可以**前向引用**文件中后声明的类型（与直接写出该类型相同）。
- 别名可以引用**另一个别名**，但后者必须在文件中**更早**声明（文本
  顺序）；前向的别名引用是编译错误。
- 别名不得与同一翻译单元内的 class、函数、其他别名、内建类型名
  （`List`、`Dict`、`Func`、`int`、……）或保留的标准库命名空间
  （`math`、`io`、`fs`）撞名。
- 开命名空间的 `using Foo;` 形式（无 `=`）保持不变，与此无关。

**限制：**
- 右侧必须是普通类型形式（类型名、泛型实例化、数组后缀）。不支持
  **成员路径**（`using X = ns.Inner;`）——文法的类型形式只接受标识
  符，因此会以解析器语法错误失败。
- 别名只能出现在类型位置；值表达式永远不会解析到别名。
- 锚定在使用点的诊断有时会指向 `using` 行（展开克隆优先采用别名
  目标的位置）。
