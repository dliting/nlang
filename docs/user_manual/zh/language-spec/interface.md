# 接口


`interface`声明一组方法签名，没有字段、没有实现。类用`implements`关键字声明符合接口并提供实现。接口类型的值是**引用**，指向实现对象（引用语义，与class相同——见[类型语义](type-semantics.md)）。

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

### 创建接口值

```nlang
Square sq = Square();
IShape a = sq;         // 上转型：接口引用指向实现类对象
IShape b = Square();   // 构造调用结果直接赋给接口变量
IShape n = null;       // 接口引用可为null
```

接口值没有独立实例——总是指向实现类对象。上转型赋值（来自实例变量或构造调用）不需要任何强转写法。`n == null`直接判空；对null接口引用访问成员（`n.Area()`）在运行期抛`null reference error`并附回溯。

### 接口支持

- **仅方法签名**——无字段、无实现。
- **`class X implements IShape`**——类用`implements`关键字声明符合接口。`:`形式用于类继承，**不用于**接口符合；`class X : IShape`会被拒绝。
- **对接口类型的局部变量/参数/字段虚分派**——经接口引用调用方法会分派到实现对象的覆写（与class虚分派同机制；见[类](class.md)「虚方法与覆写」）。
- **多态集合**——`List<IShape>`可持有`Square`/`Circle`等混合实现类对象，`foreach`遍历经接口引用调用方法，虚分派到各自的实现（`Dict`的键位与值位同样接受接口类型实参）。见[内建泛型类](builtin-generic-classes.md)。

### 实现契约

类声明`implements`后，编译器在类声明处校验实现完整性：接口的每个方法必须由继承链上**分派序首位**的同名方法以**完全一致的签名**实现（参数类型与个数、返回类型；`out`修饰亦须一致；访问级别不限，非虚方法与父类链上的实现均有效）。缺失或签名不符都是编译错误，消息形如`The class "F" does not implement the method "val(Int32): Int32" required by interface "IF".`。同名方法不得与接口契约重载并存（虚分派按名进行，重载会截走分派）。

### 必须写`public`关键字

接口方法声明为`public int m();`。NLang的默认访问修饰符是`private`；声明成`int m();`（无`public`）的接口方法能通过解析，但按private对待，调用点**访问不到**。由此得到的错误消息——"The function X does not
exist or is not accessible"——有误导性：方法存在，只是不公开。接口里请始终写`public int m();`。
