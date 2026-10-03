# 声明


### import 声明

```nlang
import io;                 // 标准库包
import lib;                // 外部模块 lib.ncu / lib.npkg
import utils.helper;       // 项目文件 utils/helper.n
import utils.*;            // 递归通配符
```

`import` 声明**本文件**可以引用哪些模块——导入集属于所在翻译单元，
从不会泄漏给其他文件。三种来源共用一套语法：

| 来源 | 模块路径 | 示例 |
|---|---|---|
| 项目文件 | 相对 `.nproj` 根的点分路径：目录路径 + 文件名主干 | `utils/helper.n` → `utils.helper`；根目录 `main.n` → `main` |
| 外部模块 | 文件名主干（单段）：`.ncu` 文件，或恰好暴露一个模块的 `.npkg` | `lib.ncu` / `lib.npkg` → `lib` |
| 标准库包 | `io` / `math` / `fs`（随工具链分发的库源） | `io` |

可见性：

| 引用 | 需要导入？ | 调用形式 |
|---|---|---|
| 同一文件 | 否 | 裸名 |
| 同目录其他项目文件 | 否（隐式） | 裸名**或**限定名 |
| 跨目录、同一项目 | **是**（`import utils.helper;` 或 `import utils.*;`） | 仅限定名：`utils.helper.f()` |
| 外部模块 | **是**（`import lib;`） | 仅限定名：`lib.f()` |
| 标准库 `io`/`math`/`fs` | **是**（`import io;`） | 限定名：`io.print` |

- 裸名解析只覆盖本文件与同目录文件；其余一律按模块路径限定。无属主
  符号（`Exception` 类家族等根级内建、原生宿主绑定）保持全局裸名
  可见。
- 通配符 `import utils.*;` 是模块路径空间中的**递归前缀匹配**：以
  `utils.` 开头的每条路径都可导入（`utils.helper`、`utils.sub.x`、
  ……）。它只缩写导入清单——调用时仍写完整路径。通配符只匹配项目
  文件；外部模块名是单段的，永不匹配。
- `import utils;` 只匹配根文件 `utils.n`；要访问 `utils/` 目录请用
  完整路径或通配符。
- 重复导入是幂等的；精确导入与通配符重叠时取并集；导入自身模块
  路径或同目录文件是无害的冗余。
- 导入目标的解析顺序：编译进本次构建的模块（项目文件，或搜索根上的
  库源——标准库与第三方源码库同一机制）→ 外部模块（`.ncu`/`.npkg`，
  经 `-I`）。没有隐式回退。
- 同一次构建内两个单元解析出同一个点分包名是编译错误，诊断会指名
  两条来源路径。名为 `io`/`math`/`fs` 的项目目录就是普通目录；每个包名
  只能存在一份。带点导入会按匹配到的搜索根解析库源（`-I <根>` ＋
  `<根>/a/b/c.n` 即可寻址 `import a.b.c;`）；预编译的带点包
  在后续阶段之前仍按末段主干搜索。
- 外部模块的代码不进产物：消费方映像只记录导入槽，运行期由加载器
  沿同一搜索路径重新定位它（机制见[命令行工具/ncc](../cli-tools/ncc.md)
  的「产物与加载期链接」）。

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

struct 变量零初始化（所有字段 = 0）。class 变量默认为 null。

### enum 声明

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

枚举值在运行期是 int32。成员可显式赋值，也可自动递增。

#### 枚举方法

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
- 枚举数组：不能对数组本身调用方法——先索引出元素（`a[i].rank()`，
  不是 `a.rank()`）。

### struct 声明

```nlang
struct Point {
    int x;
    int y;
}

int main() {
    Point p;                       // 零初始化: x = 0, y = 0
    p.x = 3;
    p.y = 4;
    Point q = p;                   // 深拷贝: q.x、q.y 独立于 p
    q.x = 9;
    Point r = new Point{x: 1, y: 2};   // 具名初始化
    return p.x + p.y + q.x + q.y + r.x + r.y;   // 3+4+9+4+1+2 = 23
}
```

struct 是值类型：
- 声明变量（`Point p;`）会把每个字段零初始化。
- 拷贝（`Point q = p;`）按值复制整个结构体，包括嵌套 struct 字段，
  因此源与拷贝相互独立。
- 字段通过成员表达式 `p.x` 读写；struct 没有方法体，因此没有可绑定的
  `this`。
- 具名初始化（`new Point{x: 1, y: 2}`）设置具名字段；未列出的字段保持
  零值。

struct 可以包含：
- 基本类型字段（int、float、string 等）
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
`[类型ID, 祖先字段..., 父类字段..., 自身字段...]`。
首格的类型 ID 标识运行期类，供虚分派使用。

**构造函数行为**：只调用本类自己的构造函数；祖先构造函数不会被自动
调用。子类构造函数可用 `super(args);` 转发到直接父类的构造函数
（见下文「super()——构造函数链」）。没有显式 `super()` 时，从祖先
继承的字段零初始化。

**隐式 `this.field`（裸成员访问）**：在方法或构造函数内，解析到
外层类字段（含继承字段）的裸标识符是隐式 `this.field` 访问。读、
赋值、复合赋值（`v += 1`）与默认参数表达式
（`int add(int x, int y = v)`）都适用。同名局部变量或参数会遮蔽
字段，与 Java/C# 语义一致。

### 虚方法与覆写

```nlang
class Animal {
    public virtual int Sound() {
        return 0;
    }
}
class Dog : Animal {
    public int Sound() {
        return 1;
    }
}
```

`virtual` 关键字把方法标记为**虚方法**：经引用调用该方法时，运行期
VM 从对象的实际类型出发沿类层次上溯，**最派生实现优先**（动态分派）。
对象布局首格的类型 ID 即供虚分派使用（见上文「class 声明」）。

**覆写**：子类声明与父类虚方法**同名同签名**的方法即构成覆写——没有
`override` 关键字（不同于 C++ 的 `override` 或 Java 的 `@Override`），
按名与签名匹配由编译器判定。

`Object` 的两个虚方法（`equals`/`getHashCode`）同样以子类声明同名方法
的方式覆写（示例见本页「隐式 Object 基类」节）。接口的虚分派同机制
（见本页「interface 声明」的 `TotalArea` 示例）；速览见
[类与继承](../getting-started/classes-and-inheritance.md)。

### interface 声明

```nlang
interface IShape {
    public int Area();
    public int Perimeter();
}

class Square implements IShape {
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
- `class X implements IShape`——类用 `implements` 关键字声明符合接口
  （`:` 形式用于类继承，不用于接口符合；`class X : IShape` 会被拒绝）
- 对接口类型的局部变量/参数/字段虚分派
- 多态集合（混合 `Square`/`Circle` 的 `List<IShape>`）

**接口声明中方法的 `public` 关键字必须写**。NLang 的默认访问修饰符
是 `private`；声明成 `int m();`（无 `public`）的接口方法能通过解析，
但按 private 对待，调用点**访问不到**。由此得到的错误消息——
"The function X does not exist or is not accessible"——有误导性：
方法存在，只是不公开。接口里请始终写 `public int m();`。

### 隐式 `Object` 基类

凡未显式继承其他类的 class 都隐式继承 `Object`。Object 由编译器
合成——没有源码级的 `class Object { ... }` 声明，用户也不写
`class Foo : Object`（该语法被拒绝）。

Object 提供两个带默认恒等语义的虚方法：

```nlang
int equals(Object other);    // 恒等：同一堆引用 → 1，否则 0
int getHashCode();           // 恒等：`this` 的堆索引（null 为 0）
```

`equals` 与 `getHashCode` 经按名分派实现虚方法——子类只需声明同名
方法即可覆写（不需要 `override` 关键字；运行期沿类层次上溯，最先
命中最派生的实现）：

```nlang
class Point {
    public int x;
    public int y;
    int getHashCode() {              // 覆写 Object.getHashCode
        return this.x * 31 + this.y;
    }
}
```

**string 的值语义**：string 虽是基本类型，但 `string.getHashCode()`
与 `string.equals(string)` 调用被内建化为*值*语义（哈希用
`std::hash`，equals 用内容比较）。这使 string 无需包装类即可用作
Dict 键。

**`==` 运算符不受 `equals` 影响**：Object.equals 是可选实现（opt-in）
的方法。class 引用上的 `==` 运算符直接比较堆索引。`equals` 单独存在
的原因，是允许用户类以值相等覆写它，而不破坏更大代码库中恒等相等
测试。

**装箱（基本类型 → Object）**：基本类型值（int / float / string）
赋给 Object 类型的目标时被隐式装箱：

```nlang
Object o = 5;            // int 装箱
Object f = 3.14;         // float 装箱
Object s = "hi";         // string 装箱

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 在调用点装箱
```

运行期表示是带标签的装箱槽位（slot[0] = 类型标签，
slot[1] = 值位）。装箱槽位不持有引用，GC 标记阶段显式跳过它们。

**`null` 字面量的装箱保持**：字面量 `0`（用作 `null`）使 OP_Box
短路——不分配堆槽位，值 `0` 原样留在 Object 槽位内容里。这让
`Object o = null` 与 `Object o = 0` 都成为无操作，而不是把 0 包进
装箱 int 的堆引用。

**拆箱与 class 向下转换（`as` 运算符）**：

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
`(T)expr` 无法与带括号表达式可靠区分（解析器分不清
`(foo) + bar` 与 `(foo + bar)`）。`as` 这类关键字运算符没有这种
歧义。C#、TypeScript 与 Kotlin 采取同一思路。

`as` 支持的转换：
- 同类型——无操作（如同一种基本类型或同一个 class）
- 装箱——基本类型到 Object（与隐式装箱路径对称）
- 拆箱——Object 到基本类型（经 `OP_Unbox` 做运行期标签检查）
- class 向下转换——Object 到子类（经 `OP_CheckCast` 做运行期类检查，
  沿堆槽位的父类链上溯）

其他转换（如 `int as float`、`int as string`）是编译错误——请用
基本类型强制转换 / `ToString()` 路径。

**Object 向上转换特例**：隐式继承的 `Object` 不会出现在源码的继承
声明里。转换检查器对
`target == Object`（任何 class 向上转换都是同类型，无操作）与
`source == Object`（任何 class 向下转换都按向下转换处理）做了特例
处理，因此 `Object o = somePoint;` 与 `o as Point` 无需在继承声明中
提及 Object 即可工作。

### 类型别名

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
  （`List`、`Dict`、`Func`、`int`、……）或已索引的库包名（标准库
  `math`/`io`/`fs`，以及搜索路径上发现的第三方库包）撞名。
- 开命名空间的 `using Foo;` 形式（无 `=`）保持不变，与此无关。

**限制：**
- 右侧必须是**基本类型名**（`int`、`float`、`string` 等）、**数组类型**
  （`int[]`）、**泛型实例化**（`List<int>`）或**函数类型**
  （`Func<int, int>`）。裸的 **class/struct/enum 类型名**
  （`using X = Counter;`）不是合法目标——会以解析器语法错误失败。
  **成员路径**（`using X = ns.Inner;`）同样不支持——文法的类型形式只
  接受标识符，因此会以解析器语法错误失败。
- 别名只能出现在类型位置；值表达式永远不会解析到别名。
- 锚定在使用点的诊断有时会指向 `using` 行（展开克隆优先采用别名
  目标的位置）。
