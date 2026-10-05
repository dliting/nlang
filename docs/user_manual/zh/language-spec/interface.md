# 接口


`interface`声明一组方法签名，没有字段、没有实现。类用`implements`关键字
声明符合接口并提供实现。接口类型的值是**引用**，指向实现对象（引用语义，
与class相同——见[类型语义](type-semantics.md)）。

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

### 接口支持

- **仅方法签名**——无字段、无实现。
- **`class X implements IShape`**——类用`implements`关键字声明符合接口。
  `:`形式用于类继承，**不用于**接口符合；`class X : IShape`会被拒绝。
- **对接口类型的局部变量/参数/字段虚分派**——经接口引用调用方法会分派到
  实现对象的覆写（与class虚分派同机制；见[类](class.md)「虚方法与覆写」）。
- **多态集合**——`List<IShape>`可持有`Square`/`Circle`混合对象。
  见[内建泛型类](builtin-generic-classes.md)。

### 必须写`public`关键字

接口方法声明为`public int m();`。NLang的默认访问修饰符是`private`；
声明成`int m();`（无`public`）的接口方法能通过解析，但按private对待，
调用点**访问不到**。由此得到的错误消息——"The function X does not
exist or is not accessible"——有误导性：方法存在，只是不公开。接口里请
始终写`public int m();`。
