# 枚举

规范性规则见规范§4.5与§8.4。

`enum`声明一组具名整数常量。枚举值在运行期是`int32`。

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

成员可显式赋值，也可自动递增（从0起）；显式成员值必须非负（`enum Neg { A = -1 }`报`enum member "A" has negative value -1; enum member values must be non-negative`）。

### 创建枚举值

```nlang
Color c = Color.Green;     // 限定成员名
Color d = 1;               // int隐式转换到enum
int n = c;                 // enum隐式取底层int值，这里是1
Color e;                   // 不初始化：底层int值0（自增enum即首个成员）
```

- 声明时用**限定成员名**`Color.Green`。
- int到enum也隐式允许（`Color d = 1;`），显式形式是`1 as Color`。底座值是否落在成员上**编译期不检查**——`toString()`在运行期才暴露：超出名字表的值抛错，被显式编号跳过的值转成空串（见下文「输出」）。
- enum到int隐式取底座值（见[类型语义](type-semantics.md)）。
- 只声明不初始化（`Color e;`）得到底座值0——自动递增的enum恰好是首个成员；显式赋非零值的enum默认0可能不落在任何成员上（见[声明](declarations.md)的默认值表）。
- 打印enum变量须先`.toString()`——`io.print`不直接接收enum变量实参；成员常量例外，`io.print(Color.Red)`直接打印成员名`Red`（见[标准库](standard-library.md)的打印支持面）。

### 值语义

枚举是以int32为底层类型的**值类型**：

- **赋值 / 传参 / 返回**：拷贝int32值。两个变量持有独立值；修改一个绝不影响另一个。
- **比较**：按整数值比较——`==`/`!=`与关系运算符都比较底层int32（见[运算符](operators.md)）。
- **无对象身份**：枚举值没有堆身份。`Color.Red`就是int32值`0`（或它被赋的值），没有可取哈希或按引用比较的对象。对枚举的`switch`按int值匹配（见[switch](switch.md)）。
- **在聚合内**：struct/class中enum类型的字段按int32存储（见[结构体](struct.md)、[类](class.md)）。

### 枚举方法

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

方法声明在成员列表之后，以`;`分隔。只有孤立结尾`;`而无方法的`enum E { A; }`也接受（Java式收尾语法）。每个方法都必须有方法体（抽象方法被拒绝）。

- **`this`就是枚举值。**方法体内`this`是接收者的int32值：可直接参与算术与`switch`（`this * base`、`switch (this)`），没有装箱。方法调用总是静态绑定——枚举没有继承，也没有虚分派。
- **经接收者调用。**`c.weight(3)`、`this.weight(3)`、`Color.Red.weight(3)`
  ——任何枚举值表达式都可以。裸写的`weight(3)`（无接收者）是编译错误，与class方法一致。
- **参数：**不支持默认值（`int f(int a, int b = 5)`被拒绝），也不支持`out`参数。
- **`toString`保留。**`toString()`内建函数（值 → 名字）不能被用户方法遮蔽。
- **`this.<成员>`按常量解析。**方法内的`this.Red`读的是成员`Red`的值——不与接收者比较。方便，但容易误读；拿不准时请显式命名接收者。
- **成员/方法同名即冲突**（`enum E { f; int f() {...} }`被拒绝）。方法默认`public`——方法是枚举值表面的一部分；显式写出`private`或`protected`则遵循与class方法相同的可见性规则（类外调用非`public`方法报`The function "..." does not exist or is not accessible.`）。
- 枚举数组：不能对数组本身调用方法——先索引出元素（`a[i].rank()`，不是`a.rank()`）。见[数组](array.md)。

### 输出

`Color.Red.toString()`返回`"Red"`（不是`"0"`）。编译器内嵌每个枚举一张名字表；VM用`OP_Enum_to_str`按值查成员名。超出名字表的值在`toString()`处抛宿主级运行期错误——不是可捕获的nlang异常；在表内但没有成员承载的值（显式成员值跳过的编号）转成空串。完整`toString`协议见[类型强制转换](type-casts.md)。

### 跨模块枚举

`enum`是完整的模块成员：导入包后，其类型、枚举值、成员方法与`toString`都能经包名使用（见[包](packages.md)）。

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

- **限定类型引用**：`lib.Color`在任意类型位置（局部变量、字段、形参、返回类型）命名导入的枚举类型。
- **枚举值是编译期常量**：`lib.Color.Red`折叠为其底座int32，与非限定的`Color.Red`完全一致。
- **成员方法两种接收者形态都可用**——变量（`c.rank()`）或成员常量（`lib.Color.Blue.rank()`）——分派方式与定义模块内相同（静态分派）。
- **`toString`可用**：`lib.Color.Green.toString()`返回`"Green"`。
- **可见性到哪都是同一套规则**：`private`成员方法跨模块边界不可达。
