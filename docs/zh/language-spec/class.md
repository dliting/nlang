# 类


`class` 是引用类型对象：它住在堆上，经引用（堆索引）访问。它携带字段、构造
函数、方法，以及单条继承线。

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

### 引用语义

class 在整个语言中遵循**引用语义**：

- **赋值**（`obj2 = obj1`）复制引用（堆索引）。两个变量指向同一对象。
- **参数传递**：class 实参传递引用。被调方可以修改对象字段，调用方能看到
  改动。
- **返回值**：返回引用，不做拷贝。
- **作为 struct 字段**：struct 存的是引用（堆索引）；struct 拷贝会浅拷贝
  该引用。见 [结构体](struct.md)。

完整的值 vs 引用摘要见 [类型语义](type-semantics.md)。

### 字段与访问修饰符

字段带访问修饰符 `public` / `private` / `protected`。NLang 的默认访问
修饰符是 `private`。

### 继承布局

对象内存布局是 `[类型ID, 祖先字段..., 父类字段..., 自身字段...]`。首格
的类型 ID 标识运行期类，供虚分派使用。

### 构造函数

构造函数可选，与类同名。只调用本类自己的构造函数；祖先构造函数不会被
自动调用。子类构造函数可用 `super(args);` 转发到直接父类的构造函数
（见下文「super()——构造函数链」）。没有显式 `super()` 时，从祖先继承
的字段零初始化。

**隐式 `this.field`（裸成员访问）**：在方法或构造函数内，解析到外层类
字段（含继承字段）的裸标识符是隐式 `this.field` 访问。读、赋值、复合
赋值（`v += 1`）与默认参数表达式（`int add(int x, int y = v)`）都适用。
同名局部变量或参数会遮蔽字段，与 Java/C# 语义一致。

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

`virtual` 关键字把方法标记为**虚方法**：经引用调用该方法时，运行期 VM
从对象的实际类型出发沿类层次上溯，**最派生实现优先**（动态分派）。对象
布局首格的类型 ID 即供虚分派使用。

**覆写**：子类声明与父类虚方法**同名同签名**的方法即构成覆写——没有
`override` 关键字（不同于 C++ 的 `override` 或 Java 的 `@Override`），
按名与签名匹配由编译器判定。

`Object` 的两个虚方法（`equals`/`getHashCode`）同样以子类声明同名方法的
方式覆写（见 [Object 与装箱](object.md)）。接口的虚分派同机制
（见 [接口](interface.md)）。速览：
[类与继承](../getting-started/classes-and-inheritance.md)。

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

- `super(args);` 在同一个 `this` 对象上调用**直接父类的构造函数**。只在
  有父类的类（`Object` 无父类）的构造函数内有效。
- 可出现在 ctor 的**任意语句位置**（不限第一条）。
- 实参个数必须与父类构造函数的形参个数匹配。对内建 Exception 家族的
  父类，构造函数恰好接收一个 `message` 实参。
- 具名实参（`super(x = 1)`）不支持（编译错误）。
- 对无构造函数的父类，`super()` 无参是合法无操作；此时传参是编译错误。

### Null 检查

class 类型变量可为 null（表示为堆索引 0）。对 null class 引用访问字段或
方法会抛出 `NullPointerException`，可被 `try/catch` 块捕获。未捕获时程序
以退出码 1 终止。见 [异常](exception.md)。string 句柄的 null 读作空串
而非抛错——见 [字符串](string.md)。
