# 速览：类与继承

本页用可运行片段速览类与继承；每节正文给出实际输出与退出码，末尾链接规范对应章节。

### 类与构造器

```nlang
import io;

class Animal {
    public string name;

    public Animal(string name) {    // 构造器与类同名
        this.name = name;
    }

    public virtual int legs() { return 0; }

    public string toString() { return name; }
}

class Dog : Animal {
    public Dog() {
        super("dog");               // 调用基类构造器
    }

    public int legs() { return 4; } // 覆写
}

int main() {
    Animal a = Dog();           // 基类引用，虚分派
    io.print(a + " has " + a.legs() + " legs");
    if (a.legs() == 4)
        return 4;
    return 1;
}
```

输出`dog has 4 legs`，退出码4。类是引用类型；`toString()`覆写后，对象可直接参与字符串拼接。引用语义与值语义的分界见[规范/类型](../specification/04-types/index.md)。

详见[规范/类](../specification/04-types/class.md)。

### 静态成员

```nlang
import io;

class Registry {
    public static int count = 0;       // 类自身上的一个槽位

    public static void note() {
        Registry.count = Registry.count + 1;
    }
}

int main() {
    Registry.note();                   // 首次使用运行初始化器
    Registry.note();
    io.print("counted " + Registry.count);
    if (Registry.count == 2)
        return 2;
    return 1;
}
```

输出`counted 2`，退出码2。`static`字段是类上的一个存储槽位——所有方法、所有实例都经类名访问同一个槽位。静态成员懒惰初始化：初始化器在任意静态成员的首次使用时运行，恰好一次，与对象数量无关。`static const`在初始化之后只读。函数体内也可以声明`static int last = 0;`——静态局部变量，行为同样是懒惰、每个声明点一次。

详见[规范/类](../specification/04-types/class.md)、[规范/语句](../specification/06-statements/index.md)。

### 接口

```nlang
import io;

interface IShape {
    int Area();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
}

int total(IShape s) {
    return s.Area();              // 虚分派
}

int main() {
    Square sq = Square();
    sq.side = 4;                  // 通过具体类型设置
    IShape shape = sq;             // 向上转型为接口
    io.print(total(shape));        // 16
    if (total(shape) == 16)
        return 16;
    return 1;
}
```

输出`16`，退出码16。接口声明实例方法签名（没有实例字段、没有实例方法体），接口类型自身还可携带带方法体的`public static`方法与`static const`常量。类用`class X implements IShape`声明实现；接口类型的变量可持有任意实现对象，调用会虚分派到运行时类。接口实例成员的默认访问级别是`public`（与类成员的`private`默认相反）；其余修饰符（`private`、`virtual`等）会被拒绝。

详见[规范/接口](../specification/04-types/interface.md)。

