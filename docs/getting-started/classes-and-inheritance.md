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

