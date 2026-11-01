> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 4.9 接口

## 4.9.1 声明与实现契约

接口是用`interface`声明的引用类型（§2.4）。它声明实例方法签名与常量声明；不携带实例字段，也不携带实例实现。

```nlang
interface IShape {
    int Area();
    int Perimeter();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
    public int Perimeter() { return 4 * this.side; }
}
```

类用`implements I`声明遵从（§2.4）；其后可以跟一个接口列表，而`:`基类形式不命名接口。编译器在类声明处校验契约：接口的每个实例方法都必须由在类的继承链上分派将选中的同名方法实现，且签名完全一致（§4.8.9）。任何访问级别都可以实现方法，非虚方法或父类的方法也算作实现。实现缺失或不匹配是编译期错误。

*资料性：*诊断会点名缺口，例如`The class "F" does not implement the method "val(Int32): Int32" required by interface "IF".`。同名方法不得作为接口契约的重载并存，因为重载会截走按名的分派。

## 4.9.2 接口成员

接口的实例成员是public抽象签名：访问修饰符默认`public`，写出`public`是冗余但合法的；`private`、`protected`、`const`、`native`与`virtual`在实例成员上是编译期错误，带体的实例成员也是编译期错误。实现类的对应方法则不受访问级别限制（§4.9.1）。

*资料性：*诊断会点名涉事成员，例如`The interface member "m" must be public.`与`The interface instance method "m" cannot have a body`。

## 4.9.3 静态成员

接口可以声明`public static`成员：静态方法带体、经接口名调用；静态常量的形式是`static const T Name = e;`，其中`e`是编译期常量；两者都属于接口类型而非任何实现类，且不参与实现约定。当参数表不同时，接口的静态方法与实例方法可以同名；完全相撞是编译期错误，且常量不能与方法同名。

```nlang
interface IMath {
    static int twice(int x) { return x * 2; }
    static int quad(int x) { return twice(twice(x)); }  // 接口内裸调用
    static const int base = 8;
    int sign(int x);                                    // 与静态并存的实例签名
}
```

静态方法必须带体，其内没有`this`。经接口名调用它（`IMath.twice(21)`），在接口可见的任意作用域都合法，包括导入的模块。静态常量的值必须折叠为编译期标量或字符串常量——与默认参数值同规则（§7.2）——且与声明类型相容；它经接口名读取（`IMath.base`），对其赋值是编译期错误。

## 4.9.4 值与分派

接口类型的值是实现该接口的类的某个实例的引用，或`null`。默认值是`null`。接口值没有独立实例：它总是指代一个实现对象，且从类到接口类型的转换是一次隐式向上转型（§4.4）。

```nlang
Square sq = Square();
IShape a = sq;         // 上转型：接口引用指向实现类对象
IShape b = Square();   // 构造调用结果直接赋给接口变量
IShape n = null;       // 接口引用可为null
```

经由接口引用的调用按对象的运行时类做动态分派，机制与类虚分派相同（§4.8.9）。经由`null`接口引用调用方法时，是可在执行期捕获的错误（§4.8.10，第8章）。

接口类型独立于其他一切类型：它只接受实现它的类的引用与`null`字面量，且它的值不隐式转换到任何东西——既不转换到`Object`，也不转换到另一个接口（§4.4）。

*资料性：*接口类型实参构成多态集合：`List<IShape>`可持有混合的实现对象，经所存引用的每次调用都分派到各自的实现（§4.12.6）。
