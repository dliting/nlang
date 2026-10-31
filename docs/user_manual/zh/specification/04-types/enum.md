> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 4.6 枚举

## 4.6.1 声明与值

枚举是用`enum`声明的一个独特类型（§2.4）。它有一组固定的命名*成员*，每个成员有一个非负整数值：显式给出的值，或比前一个成员大一（从0起算）。显式的负值是编译期错误。

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

枚举的表示是`int`：该类型的值可以是任意`int`，不限于已声明的成员。默认值是0。枚举与`int`之间可隐式互转（§4.4），并以整数值装箱进入`Object`（§4.11）。

```nlang
Color c = Color.Green;     // 限定成员名
Color d = 1;               // int隐式转换到enum
int n = c;                 // enum给出其底层值，这里是1
Color e;                   // 不初始化：底层值0
```

`int`转换到枚举时，底座值是否落在某个已声明成员上，编译期不检查：无论名字表如何，`Color d = 1;`与显式的`1 as Color`都被接受；没有成员携带的值在`toString()`处报告什么，见§4.6.3。

枚举是值类型：赋值、参数传递与返回复制底层整数值，因此两个变量此后持有独立的值。值按整数值比较——`==`、`!=`与关系运算符都比较表示——且枚举没有对象身份：对枚举的`switch`按整数值匹配（§6.3）。枚举类型的结构体或类字段存储该整数值（§4.7，§4.8）。

## 4.6.2 枚举方法

枚举可以声明方法。方法声明在成员列表之后，以`;`与成员列表分隔；只有结尾`;`而无方法也被接受。每个方法都必须有方法体。

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

在枚举方法内，`this`是接收者的底层`int32`值：它直接参与算术与`switch`（§5.2，§6.3）。方法经接收者表达式调用——变量、成员常量或任意枚举值表达式；无接收者的调用是编译期错误。方法静态分派：枚举没有继承，也没有虚分派。枚举方法不接受默认实参，也不接受`out`参数（§7.2）。内建`toString`是保留名：用户方法不能遮蔽它（§4.6.3）。在方法内，对成员`M`写作`this.M`给出`M`的常量值，而非与接收者比较。成员名与方法名不得冲突。方法默认`public`；`private`与`protected`遵循§4.8.2的可见性规则。

*资料性：*`this.M`按常量解析虽方便却容易误读；显式命名接收者即可消除歧义。对枚举数组调用方法要先索引元素：`a[i].weight(3)`，而不是`a.weight(3)`（§4.10）。

## 4.6.3 toString与输出

对枚举值调用`toString()`返回携带该值的成员的名字。落在成员表内但没有任何成员携带的值返回空串；超出表的值在`toString()`处抛出宿主级执行期错误（第9章区分宿主级错误与异常）。

```nlang
string s = Color.Red.toString();   // "Red"
```

*资料性：*编译器为每个枚举内嵌一张名字表，运行期系统按值查找成员名。打印枚举变量要经由`.toString()`：在期望文本处传入枚举变量是编译期错误，而成员常量直接打印其名字。

## 4.6.4 跨模块枚举

枚举是完整的模块成员：导入包后，其类型、枚举值、成员方法与`toString`都能经包名使用（[包](../03-program-structure/packages.md)）。

```nlang lib.n
enum Color {
    Red, Green, Blue;

    public int rank() { return this * 10; }
}
```

```nlang
import lib;

lib.Color c = lib.Color.Green;          // 限定类型＋限定成员值
int r = c.rank();                       // 经变量接收者调方法
int r2 = lib.Color.Blue.rank();         // 经成员常量调方法
string s = lib.Color.Red.toString();    // 内建toString → "Red"
```

`lib.Color`在任意类型位置——局部变量、字段、形参与返回类型——命名导入的枚举类型。限定成员常量（如`lib.Color.Red`）是编译期常量，折叠为其底层值，与非限定的`Color.Red`完全一致。成员方法两种接收者形态都可用——变量（`c.rank()`）或成员常量（`lib.Color.Blue.rank()`）——分派方式与定义模块内相同（静态分派），且`toString`返回成员名。可见性到哪都是同一套规则：`private`成员方法跨模块边界不可达（§4.8.2）。
