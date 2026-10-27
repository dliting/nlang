# 结构体

规范性规则见规范§4.6与§8.5。

`struct`是值类型聚合，由字段组成。它持有数据加上可选构造器——没有一般方法体。需要行为请用`class`。

```nlang
struct Point {
    int x;
    int y;
}
```

struct成员始终公开可访问：字段与构造器默认`public`，可显式写出的修饰符也只有`public`本身——struct成员写出`private`或`protected`是编译错误（`The struct member "%s" must be public.`）。struct是无隐藏状态的纯数据记录，不适用可见性规则。

### 值语义

struct是具**深拷贝**语义的**值类型**，贯穿整个语言：

- **声明**（`Point p;`）会给每个字段赋予默认值——未声明初始化器时为0，声明了则为所声明的默认值（见下文「字段初始化器」）。
- **赋值 / 拷贝**（`Point q = p;`）按值复制整个结构体，包括嵌套struct字段，因此源与拷贝相互独立。
- **参数传递 / 返回**：传参与返回各深拷贝一次。被调方操作的是自己那份副本。
- **作为class字段**：class持有一份独立的深拷贝。`obj.s = s1`会把`s1`深拷贝进该class的字段。
- **作为数组元素**：`Point[n]`会在创建时为每个元素分配一个全新、独立的struct实例（含嵌套struct字段，递归进行）。把元素读入struct变量（`Point p = arr[i]`）时深拷贝；经下标写入（`arr[i].x = v`、`arr[i] = p`）则存入数组自己的元素。零长度struct数组（`Point[0]`）合法——`.length()`为0，不创建任何元素。

**struct内class引用的浅拷贝**：struct含class类型字段时，struct拷贝会原样复制该class引用（堆索引）。原件与副本指向堆上同一个class对象。这与C#对引用类型struct字段的行为一致。

```nlang
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // 浅拷贝：b.ref == a.ref（同一对象）
    b.ref.x = 99;        // 修改共享的Inner对象
    return a.ref.x;      // 返回99，不是10
}
```

### 创建与初始化

struct可用与class相同的语法声明构造器（可选`public`修饰符、默认参数、重载以及隐式`this`裸字段访问——见[类](class.md)「构造函数」）。构造器**仅在显式构造时**运行：

```nlang
Point p(2, 3);                  // 直接初始化声明
Point q = Point(1, 4);          // 构造调用，赋值
Point r;                        // 裸声明：不运行构造器
Point s = Point{5};             // 初始化列表：不运行构造器
```

- **构造调用**`Point(1, 4)`与**直接初始化**`Point p(2, 3);`会运行构造器。`new Point(...)`同样会（值被放到堆上，与class类型的`new`一致——见[类](class.md)）。
- **裸声明**`Point r;`与**初始化列表**`Point{...}`**不**运行构造器——每个字段取默认值（未声明初始化器时为0，声明了则为所声明的默认值）。数组元素、嵌套struct字段、深拷贝行为相同。
- **具名条目**`Point{x: 1, y: 2}`：按字段名赋值，未列出的字段保持默认值。
- **位置条目**`Point{3, 7}`：按字段声明顺序赋值；可以只给前几个——`Point{5}`只填`x`，其余字段保持默认值。
- struct数组`Point[3]`在创建时为每个元素分配全新实例，见上文值语义与[数组](array.md)。

字段通过成员表达式`p.x`读写；构造器内可用`this`指名一个字段。完整初始化列表规则（目标形状与消歧）见[集合初始化器](collection-initializers.md)。

### 字段初始化器

struct字段可带编译期常量默认值，与class字段相同（见[类](class.md)「字段初始化器」）：

```nlang
struct Point {
    int x = 3;
    int y = 4;
    string tag = "pt";
}
```

与class不同，struct默认值**不注入到构造器**——它在每次分配时应用。裸声明、数组元素、嵌套字段、部分初始化列表都会先填入默认值（初始化列表条目会覆盖它们）。深拷贝复制存储值，不重新应用默认值。默认值保存在`.ncu`的struct记录里，因此`import`之后依然可见——见[模块序列化](../vm-architecture/module-serialization.md)。

### struct可以包含

- 基本类型字段（int、float、string等）
- enum字段（按int32存储）
- struct字段（深拷贝，由外层struct持有）
- class字段（引用，浅拷贝）
- 构造器（与class相同的语法）

struct不能包含一般方法。需要行为请用class。

### 跨模块的struct

导入的`struct`是完整的模块成员：其`public`字段跨模块可读写，锚定花括号形式按字段名填充（共享的导入模型见[跨模块的类](class.md#跨模块的类)）。

```nlang lib.n
struct Vec {
    int x;
    int y;
}
```

```nlang
import lib;

lib.Vec v = {x: 1, y: 2};   // 按字段名花括号初始化
v.x = 10;                   // 写导入字段
int sum = v.x + v.y;        // 读导入字段
```

- **字段访问遵循到哪都相同的可见性规则**：只有`public`字段跨边界可达，`private`字段不可达（struct字段除非显式写出，否则为`public`）。
- **按名初始化与构造**：`lib.Vec v = {x: 1, y: 2};`按桩侧重建的字段列表按名填充；若struct声明了构造函数，`lib.Vec(3, 4)`跨边界调用它。
- **字段默认值在分配时照常应用**：生产方的字段默认值随`.ncu`记录携带，因此不带初始化器声明的导入struct会像在本模块内一样物化这些默认值。

### 输出

struct没有`toString`，且**永久排除**于字符串强制转换——`"x" + structInstance`是编译错误（struct在NLang中是纯数据类型）。见[类型强制转换](type-casts.md)「Object.toString()协议」。
