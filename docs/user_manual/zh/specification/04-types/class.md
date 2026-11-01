> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 4.8 类

## 4.8.1 声明与引用语义

类是用`class`声明的引用类型（§2.4）。类类型的值是对该类一个实例的引用，或`null`。默认值是`null`。类住在堆上，经其引用到达；它携带字段、构造器与方法，以及单条继承线。

```nlang
class Node {
    public int value;
    public Node next;

    public Node(int v) {
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

类是单继承：至多一个父类，在声明中命名（§4.8.5）。每个类最终都根植于`Object`（§4.11）。

类值在整个语言中遵循引用语义：

- **赋值**（`obj2 = obj1`）复制引用；两个变量此后指代同一对象。
- **参数传递**传递引用：被调方可以修改对象的字段，调用方能看到改动。
- **返回值**返回引用，不做拷贝。
- **作为结构体字段**：结构体存储该引用，结构体拷贝会浅拷贝它（§4.7.4）。

## 4.8.2 访问修饰符

不写访问修饰符的成员是`private`：类成员默认private，接口成员默认public（§4.9.2）。访问修饰符管辖类的成员：

- `public`——在类自身可见的任何地方都可到达；
- `private`——只在声明类的方法内部可到达；
- `protected`——在声明类内部、以及其子类体内部可到达，按裸名或经基类类型名到达；其他任何地方不可到达。

同样的规则跨模块边界成立：导入的类只有其`public`成员可到达，例外是子类体内的`protected`成员（§4.8.11）。不是`public`的构造器不能在类外调用。

*资料性：*诊断会点名涉事成员，例如`constructor of class "C" is not accessible`。

## 4.8.3 字段与字段初始化器

字段可以携带初始化器（§2.4）。初始化器必须是常量表达式：任意类型的字面量，允许一元取负；它不能引用字段或`this`。默认值在体运行之前注入类的每个构造器，基类在前：调用`super(...)`的构造器在该调用之后紧接着接受注入，没有`super(...)`的则在体首注入；存在字段默认值时，`super(...)`必须是构造器的第一条语句。没有声明构造器的类获得一个合成的`public`零参构造器，携带这些默认值。`native`或无体的构造器不能携带字段默认值。

```nlang
class Cfg {
    public int x = 5;
    public string s = "hi";
    public float f = -1.5;
}
```

默认值由合成的构造器携带，因此`Cfg()`与花括号形态`Cfg{}`都会应用它们。

## 4.8.4 实例创建

构造器调用`C(args)`分配对象并运行选中的构造器（§5.8）；对没有声明构造器的类，`C()`分配对象并把字段零初始化。在类体内部，类自身的名字仍然指代一次构造器调用。

```nlang
Node a = Node(42);        // 构造调用：堆上分配并运行构造器
Node b(42);               // 直接初始化声明，与上一行等价
Node c;                   // 只声明：c是null
```

锚定花括号形态`C{field: value, ...}`在构造之后按名字填充字段；该类必须有一个无参构造器，显式的或合成的——本页的`Node`带`Node(int v)`有参构造，不适用此形态。声明位形式对逗号分隔列表的每个声明符都开放；声明符的形态见§3.8。

## 4.8.5 继承与实例布局

类的基类由`: Base`给出，且继承是单继承：一个基类，没有多类继承。一个类可以实现任意多个接口，`implements I1, I2`（§4.9.1）。没有显式基类的类隐式扩展`Object`（§4.11）。

*资料性：*对象的内存布局是`[类型ID, 祖先字段..., 父类字段..., 自身字段...]`。首格的类型ID记录对象的实际类，虚分派在运行期据此选中实现（§4.8.9）。

## 4.8.6 构造器与super()

构造器是不带返回类型声明的成员（§2.4），以类的名字命名。构造器是可选的，可以重载（§7.3）。祖先的构造器不会自动调用：没有显式`super(...)`时，继承的字段保持其默认值。

构造器内的`super(args);`在同一对象上调用直接父类的构造器；它只在有父类的类的构造器中有效，只接受按位置的实参（§7.2），且其实参表必须匹配某个父构造器。父类属于内建`Exception`家族时，其构造器只接受一个`message`实参。对没有构造器的父类使用`super()`是合法的空操作；这种情况下带实参是编译期错误。

```nlang
class Base {
    public int v;
    public Base(int x) { this.v = x; }
}
class Kid : Base {
    public Kid(int x) {
        super(x * 2);        // 调用Base(int)
    }
}
```

*资料性：*`super(...)`可以出现在构造器体的任意语句位置，除非该类携带字段默认值——此时它被钉在第一条语句（§4.8.3）。`super(...)`不接受具名实参。

## 4.8.7 名字解析与this

在方法或构造器内部，指代所在类的字段——包括继承的字段——的裸标识符，是一次隐式的`this.field`访问；同名的局部或参数遮蔽该字段（§3.7）。隐式形态适用于读、赋值、复合赋值与默认形参表达式。

## 4.8.8 静态成员

`static`方法属于类而非实例：它经类型名调用，没有`this`，类内对它的裸调用直接指代它。当参数表不同时，静态方法与实例成员可以同名；签名完全相同的相撞是编译期错误。静态成员不参与虚分派（§4.8.9）；子类继承其父类的静态成员。

```nlang
class Util {
    public static int twice(int x) { return x * 2; }
    public static int quad(int x) { return twice(twice(x)); }  // 类内裸调用
    public int inst() { return 1; }
}
class Base {
    public static int bid() { return 7; }
}
class Sub : Base {
    public int pad;
}
int main() {
    return Util.twice(21) + Util.quad(2) + Sub.bid();  // 42 + 8 + 7
}
```

*资料性：*两种调用形态严格区分：经对象调用静态方法会被拒绝（`must be called through the type name`），经类型名调用实例方法同样被拒（`must be called through a receiver object`）。在静态方法内使用`this`是编译错误（`this is not available in a static method`）。

## 4.8.9 虚方法与重写

`virtual`标志标记一个方法做动态分派。经由类引用的调用，从对象的运行时类向上遍历类层次来选择实现：名字的最派生实现胜出。对非虚方法的调用绑定到静态声明的类型，从不做动态分派：与某个非虚祖先方法同名的子类方法，在经由子类类型的调用中遮蔽它，而经由祖先类型的调用仍运行祖先的实现。

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

重写不需要关键字：子类中同名同签名的方法取代继承来的实现。子类只在构造器中可以用`super(...)`调用父实现；没有`super.method(...)`形式。

与某个祖先虚方法同名的子类方法必须与其签名完全一致：相同的参数类型与个数、相同的返回类型、相同的`out`修饰。不一致是编译期错误。一旦一个名字绑定了虚约定——继承的虚方法或实现的接口方法（§4.9.1）——该名字就不得在类或其任何子类中重载：同名的各方法必须与约定一一对应。在一个类内，带`virtual`成员的名字完全不能重载；要让整个家族非虚才能重载它。`Object`协议方法是虚的，遵循同一条规则（§4.11.2）。

*资料性：*虚分派按方法名选择——调用点按静态绑定类型的签名布局实参，运行期取最近派生的同名方法——所以签名不一致会错读实参与返回值的位模式。修正签名即可重写；想保留不同签名的同名方法请改名。

## 4.8.10 同一性、相等与null

两个类引用按身份比较：当且仅当两者指代同一实例或同为`null`时`==`成立。方法`equals`是另一个独立的、需显式参与的操作（§4.11.2）；重写它不改变`==`，值相等需要显式调用`equals`。

经由`null`引用访问成员或调用方法时，是执行期错误；错误以`NullPointerException`抛出（第8章）。

*资料性：*`null`即堆索引0。未捕获的`NullPointerException`使程序以退出码1终止（第9章）。`string`句柄的`null`则读作空串（§4.3）。

## 4.8.11 跨模块的类

导入模块的类是完整类型：`import lib;`之后，`lib.B`的`public`实例字段可读写、`public`实例方法可调（含继承成员、默认参数与重载），虚调用与同单元内一样动态分派。

```nlang
import lib;
class D : lib.B {
    public int f(int x) { return x + 10; }   // 重写lib.B.f
}
int main() {
    D d = D();
    return d.f(3);
}
```

```nlang lib.n
class B {
    public virtual int f(int x) { return x; }
}
```

`class D : lib.B`继承导入的基类，`class C implements lib.I`实现导入的接口；重写与`implements`约定跨模块边界的强制与同单元完全一致。可见性到哪都是同一套规则：导入类只有`public`成员对导入方可达，`protected`成员在子类体内可达，非`public`构造器不能跨边界调用（§4.8.2）。
