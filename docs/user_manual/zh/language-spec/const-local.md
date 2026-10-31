# Const局部

规范性规则见规范§6.1。
```nlang
const int X = 5;
const string Greeting = "hello";
```

标记为`const`的局部变量必须在声明时初始化，之后不能再被赋值或复合赋值。形参也可以标记`const`（被调方不能对其重新赋值）；class/struct字段上的`const`是语法错误。一般形式见[变量声明](declarations.md)。

**const是浅的（Java-`final`风格）**：`const`防止重新绑定*名字*，但不冻结被引用对象的状态。通过const局部做成员变更是允许的：

```nlang
const Foo f = Foo();
f.x = 5;            // OK —— f本身未被重新赋值
f = Foo();      // 错误 —— 不能给const局部重新赋值
const Point p = q;
p.x = 5;            // OK —— 只有p的绑定是const
```

对class字段和数组元素而言，这意味着：`const`引用仍允许通过它写入。深/不可变风格的const不受支持。

要整个聚合不可变，用**类型级**const——`const class` / `const struct`声明（见[类](class.md)「const类与const结构体」、[结构体](struct.md)「const struct」）：实例字段构造后只读，容器字段读取返回副本。两者正交：const类型的变量本身仍可重新绑定，而本页的const局部不可以。字段声明位上的`const`（如`const int x;`作为字段）始终是语法错误——不可变性由类型级标记整体授予，不做「半个类不可变」。
