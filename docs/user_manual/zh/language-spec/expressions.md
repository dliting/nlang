# 表达式

规范性规则见规范§5.1。

表达式产生一个值。NLang是静态类型：每个表达式都有类型，编译器按类型检查每个操作。

### 表达式页面

| 表达式                                          | 页面 |
|---------------------------------------------------|------|
| 运算符（`+ - * / %`、比较、逻辑）              | [运算符](operators.md) |
| 强制转换（`as`、向string强制转换）            | [类型强制转换](type-casts.md) |
| 集合初始化器（`[...]`、`Type{...}`）        | [集合初始化器](collection-initializers.md) |

字符串插值（`${...}`）、转义序列、字符串比较是字符串字面量特性：见[字符串](string.md)。char字面量（`'a'`、`'A'`）与类型语义见[基本类型](primitives.md)。

### 成员访问

```nlang
obj.field          // 字段读
obj.field = value  // 字段写
obj.method(args)   // 方法调用
```

对class对象，`obj`必须非null（运行期null检查——见[类](class.md)「Null检查」）。struct没有一般方法——只有构造器；字段通过成员表达式读写（见[结构体](struct.md)）。enum成员是常量，不是值的成员（见[枚举](enum.md)）。

### 创建与初始化

各类型的创建形态速查（详细语义在逐类型页）：

| 类型 | 创建形态 | 详见 |
|---|---|---|
| class | 构造调用`Node(42)`；直接初始化声明`Node n(42);`；锚定花括号`P{value: 42}` | [类](class.md)「创建实例」 |
| struct | 构造调用`Point(1, 4)`；直接初始化声明`Point p(2, 3);`；具名条目`Point{x: 1, y: 2}`；位置条目`Point{3, 7}`；只声明零初始化`Point p;` | [结构体](struct.md)「创建与初始化」 |
| enum | 限定成员名`Color.Red`；int隐式转换`Color c = 1;` | [枚举](enum.md) |
| 数组 | 表达式位`int[3]`、`[1, 2, 3]`；声明位`int a[3];`、`int b[100] = [7, 8];` | [数组](array.md) |
| interface | 上转型`IShape s = sq;`——接口值总是指向实现类对象 | [接口](interface.md) |
| List/Dict | 字面量`[1, 2]`、`{"k": 1}`；空集合`{}` | [集合初始化器](collection-initializers.md) |

构造调用`Node(42)`是表达式：在堆上分配对象、按实参运行构造函数，返回引用（见[类](class.md)「构造函数」）。struct构造调用`Point(1, 4)`同样是表达式；花括号条目`Point{...}`不运行构造器、直接按字段填值（见[结构体](struct.md)「创建与初始化」）。声明位形态（`T name(args);`、`T name[n];`、`T name[n] = [...]`）在多声明符声明的每个声明符上都允许，见[声明](declarations.md)。
