# 接口


`interface`声明一组实例方法签名，没有字段、没有实例实现。类用`implements`关键字声明符合接口并提供实现。接口本身还可携带`public static`成员——带体的方法与常量——它们属于接口类型，不属于任何实现类（见下文「静态成员」）。接口类型的值是**引用**，指向实现对象（引用语义，与class相同——见[类型语义](type-semantics.md)）。

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

int TotalArea(IShape s) {
    return s.Area();   // 经接口虚分派
}
```

### 创建接口值

```nlang
Square sq = Square();
IShape a = sq;         // 上转型：接口引用指向实现类对象
IShape b = Square();   // 构造调用结果直接赋给接口变量
IShape n = null;       // 接口引用可为null
```

接口值没有独立实例——总是指向实现类对象。上转型赋值（来自实例变量或构造调用）不需要任何强转写法。`n == null`直接判空；对null接口引用访问成员（`n.Area()`）在运行期抛`NullPointerException`，可被`try/catch`捕获——见[异常](exception.md)。

### 接口支持

- **仅实例方法签名**——无实例字段、无实例实现。静态方法（带体）与静态常量属于接口本身（见下文「静态成员」）。
- **`class X implements IShape`**——类用`implements`关键字声明符合接口。`:`形式用于类继承，**不用于**接口符合；`class X : IShape`会被拒绝。
- **对接口类型的局部变量/参数/字段虚分派**——经接口引用调用方法会分派到实现对象的覆写（与class虚分派同机制；见[类](class.md)「虚方法与覆写」）。
- **多态集合**——`List<IShape>`可持有`Square`/`Circle`等混合实现类对象，`foreach`遍历经接口引用调用方法，虚分派到各自的实现（`Dict`的键位与值位同样接受接口类型实参）。见[内建泛型类](builtin-generic-classes.md)。

### 实现契约

类声明`implements`后，编译器在类声明处校验实现完整性：接口的每个方法必须由继承链上**分派序首位**的同名方法以**完全一致的签名**实现（参数类型与个数、返回类型；`out`修饰亦须一致；访问级别不限，非虚方法与父类链上的实现均有效）。缺失或签名不符都是编译错误，消息形如`The class "F" does not implement the method "val(Int32): Int32" required by interface "IF".`。同名方法不得与接口契约重载并存（虚分派按名进行，重载会截走分派）。静态成员不进入契约（见下文「静态成员」）。

### 成员修饰符

接口**实例**成员就是**public抽象签名**。缺省访问修饰符是`public`：`int m();`与`public int m();`等价，后者冗余但合法——这与类/struct/enum成员的`private`缺省相反。除此之外的修饰符在接口实例成员上一律是编译错误：`private`/`protected`（消息形如`The interface member "m" must be public.`）、`const`（方法不能是const）、`native`（签名不带实现）、`virtual`（接口方法本就按名虚分派）。带体的实例成员同样被拒（`The interface instance method "m" cannot have a body`）。实现类的对应方法访问级别则不受限（见上文实现契约）。

### 静态成员

接口可以声明`public static`成员。它们属于接口类型本身，为所有实现类共享，不进入实现契约：

```nlang
interface IMath {
    static int twice(int x) { return x * 2; }
    static int quad(int x) { return twice(twice(x)); }  // 接口内裸调用
    static const int base = 8;
    int sign(int x);                                    // 与静态并存的实例签名
}
```

- **静态方法**必须是public且必须带体（`static int f();`报`The interface member "f" is static but has no body`）。经接口名调用，`IMath.twice(21)`——任何能看到该接口的作用域均可，包括导入模块。静态方法体内按裸名直调兄弟静态方法；`this`不可用（`this is not available in a static method`）。
- **静态常量**形如`[public] static const Type Name = 常量表达式;`。值必须折叠为编译期标量或字符串常量（与默认参数值同规则），且与声明类型兼容。经接口名读取，`IMath.base`；对其赋值是编译错误。
- **独立名字空间**：静态方法与实例方法可同名并存（参数表不同），但签名完全相同是编译错误。常量不得与接口的方法同名。
