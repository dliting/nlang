# 速览: 类与继承
### 类与构造器

```nlang
import io;

class Animal {
    public string name;

    public int Animal(string name) {    // 构造器与类同名
        this.name = name;
        return 0;
    }

    public virtual int legs() { return 0; }

    public string toString() { return name; }
}

class Dog : Animal {
    public int Dog() {
        super("dog");               // 调用基类构造器
        return 0;
    }

    public int legs() { return 4; } // 覆写
}

int main() {
    Animal a = new Dog();           // 基类引用，虚分派
    io.print(a + " has " + a.legs() + " legs");
    if (a.legs() == 4)
        return 4;
    return 1;
}
```

输出 `dog has 4 legs`，退出码 4。类是引用类型；`toString()` 覆写后，
对象可直接参与字符串拼接。

详见 → [语言规格/声明](../language-spec/declarations.md)。

### 接口

```nlang
import io;

interface IShape {
    public int Area();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
}

int total(IShape s) {
    return s.Area();              // 虚分派
}

int main() {
    Square sq = new Square();
    sq.side = 4;                  // 通过具体类型设置
    IShape shape = sq;             // 向上转型为接口
    io.print(total(shape));        // 16
    if (total(shape) == 16)
        return 16;
    return 1;
}
```

输出 `16`，退出码 16。接口只声明方法签名（没有字段、没有方法体）。类用
`class X implements IShape` 声明实现；接口类型的变量可持有任意实现对象，
调用会虚分派到运行时类。接口方法必须加 `public`——默认访问级别是
`private`，未加 `public` 的方法会被解析但不允许调用。

详见 → [语言规格/声明](../language-spec/declarations.md)。

