# Const局部


```nlang
const int X = 5;
const string Greeting = "hello";
```

标记为`const`的局部变量必须在声明时初始化，之后不能再被赋值或复合赋值。仅支持**局部** const；不支持class/struct字段的const。一般形式见[变量声明](declarations.md)。

**const是浅的（Java-`final`风格）**：`const`防止重新绑定*名字*，但不冻结被引用对象的状态。通过const局部做成员变更是允许的：

```nlang
const Foo f = new Foo();
f.x = 5;            // OK —— f 本身未被重新赋值
f = new Foo();      // 错误 —— 不能给 const 局部重新赋值
const Point p = q;
p.x = 5;            // OK —— 只有 p 的绑定是 const
```

对class字段和数组元素而言，这意味着：`const`引用仍允许通过它写入。深/不可变风格的const不受支持。
