# 表达式


表达式产生一个值。NLang 是静态类型：每个表达式都有类型，编译器按类型检查每
个操作。

### 表达式页面

| 表达式                                          | 页面 |
|---------------------------------------------------|------|
| 运算符（`+ - * / %`、比较、逻辑）              | [运算符](operators.md) |
| 强制转换（`(T)`、`as`、向 string 强制转换）     | [类型强制转换](type-casts.md) |
| 集合初始化器（`[...]`、`new Type{...}`）        | [集合初始化器](collection-initializers.md) |

字符串插值（`${...}`）、转义序列、字符串比较是字符串字面量特性：见
[字符串](string.md)。

### 成员访问

```nlang
obj.field          // 字段读
obj.field = value  // 字段写
obj.method(args)   // 方法调用
```

对 class 对象，`obj` 必须非 null（运行期 null 检查——见 [类](class.md)「Null
检查」）。struct 没有方法；字段通过成员表达式读写（见 [结构体](struct.md)）。
enum 成员是常量，不是值的成员（见 [枚举](enum.md)）。

### 对象创建

```nlang
Node n = new Node();
Node n = new Node(42);
```

`new` 在堆上分配，并在存在构造函数时调用它。见 [类](class.md)「构造函数」。
数组创建是 `new T[n]`——见 [数组](array.md）。
