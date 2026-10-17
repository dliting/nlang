# 类


`class`是引用类型对象：它住在堆上，经引用（堆索引）访问。它携带字段、构造函数、方法，以及单条继承线。

```nlang
class Node {
    public int value;
    public Node next;

    public int Node(int v) {
        this.value = v;
        return 0;
    }

    public int getValue() {
        return this.value;
    }
}

class SpecialNode : Node {
    public int extra;
}
```

### 引用语义

class在整个语言中遵循**引用语义**：

- **赋值**（`obj2 = obj1`）复制引用（堆索引）。两个变量指向同一对象。
- **参数传递**：class实参传递引用。被调方可以修改对象字段，调用方能看到改动。
- **返回值**：返回引用，不做拷贝。
- **作为struct字段**：struct存的是引用（堆索引）；struct拷贝会浅拷贝该引用。见[结构体](struct.md)。

完整的值vs引用摘要见[类型语义](type-semantics.md)。

### 字段与访问修饰符

字段带访问修饰符`public` / `private` / `protected`。NLang的默认访问修饰符是`private`。

### 创建实例

```nlang
Node a = Node(42);        // 构造调用：堆上分配并运行构造函数
Node b(42);               // 直接初始化声明，与上一行等价
Node c;                   // 只声明：c是null
```

- **构造调用**`Node(42)`是表达式：在堆上分配对象、按实参运行构造函数，返回引用。没有构造函数的类写`P()`同样合法——只分配并把字段零初始化。类体内裸调用同类名不是构造调用（见[已知限制](known-limitations.md)）。
- **锚定花括号**`P{value: 42}`以`字段:值`条目填值：要求类有无参构造函数（显式或隐式）——本页`Node`带`Node(int v)`有参构造，不适用此形态；字段在构造之后按名赋值，见[集合初始化器](collection-initializers.md)。
- 声明位形式`Node b(42);`只允许出现在首个声明符上（`Node a, b(42);`是语法错误——括号臂不在首位）；定长数组声明`Node arr[3];`的规则见[已知限制](known-limitations.md)。
- 只声明不初始化（`Node c;`）得到null，见[声明](declarations.md)的默认值表与下文Null检查。

### 继承布局

对象内存布局是`[类型ID, 祖先字段..., 父类字段..., 自身字段...]`。首格的类型ID标识运行期类，供虚分派使用。

### 构造函数

构造函数可选，与类同名。只调用本类自己的构造函数；祖先构造函数不会被自动调用。子类构造函数可用`super(args);`转发到直接父类的构造函数（见下文「super()——构造函数链」）。没有显式`super()`时，从祖先继承的字段零初始化。

**隐式`this.field`（裸成员访问）**：在方法或构造函数内，解析到外层类字段（含继承字段）的裸标识符是隐式`this.field`访问。读、赋值、复合赋值（`v += 1`）与默认参数表达式（`int add(int x, int y = v)`）都适用。同名局部变量或参数会遮蔽字段，与Java/C#语义一致。

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

`virtual`关键字把方法标记为**虚方法**：经引用调用该方法时，运行期VM从对象的实际类型出发沿类层次上溯，**最派生实现优先**（动态分派）。对象布局首格的类型ID即供虚分派使用。

子类方法与祖先虚方法同名但参数或返回类型不同时，编译器报错：虚分派按方法名进行——调用点按静态绑定类型的签名布局实参，运行期取最近派生的同名方法，签名不一致会导致实参与返回值按位错读。因此名字一旦绑定虚方法契约（祖先同名虚方法，或`implements`接口的同名方法），该名字在子类就**不得重载**：子类的同名方法集必须与契约签名一一对应（完全相同的参数类型与个数、返回类型；`out`修饰亦须一致）。**同类内同理**：含`virtual`成员的方法名在类内完全不可重载——虚分派按名选择，族内签名必须唯一；要重载就让整个族都非虚。修正签名即可覆写；想保留不同签名的同名方法请改名。`Object`协议方法（`equals`/`getHashCode`/`toString`）遵循同一规则，规范签名见[Object与装箱](object.md)。

`Object`的三个虚方法（`equals`/`getHashCode`/`toString`）同样以子类声明同名方法的方式覆写（见[Object与装箱](object.md)）；`==`比较不受`equals`覆写影响——class的`==`恒为同一性，值相等须显式调用`equals`。接口的虚分派同机制（见[接口](interface.md)）。速览：[类与继承](../getting-started/classes-and-inheritance.md)。

### super()——构造函数链

```nlang
class Base {
    public int v;
    public int Base(int x) { this.v = x; return 0; }
}
class Kid : Base {
    public int Kid(int x) {
        super(x * 2);        // 调用 Base(int)
        return 0;
    }
}
```

- `super(args);`在同一个`this`对象上调用**直接父类的构造函数**。只在有父类的类（`Object`无父类）的构造函数内有效。
- 可出现在ctor的**任意语句位置**（不限第一条）。
- 实参个数必须与父类构造函数的形参个数匹配。对内建Exception家族的父类，构造函数恰好接收一个`message`实参。
- 具名实参（`super(x = 1)`）不支持（编译错误）。
- 对无构造函数的父类，`super()`无参是合法无操作；此时传参是编译错误。

### Null检查

class类型变量可为null（表示为堆索引0）。对null class引用访问字段或方法会抛出`NullPointerException`，可被`try/catch`块捕获。未捕获时程序以退出码1终止。见[异常](exception.md)。string句柄的null读作空串而非抛错——见[字符串](string.md)。
