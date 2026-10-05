# 结构体


`struct`是值类型聚合，由字段组成。它只持有数据——没有方法体、没有`this`。需要行为请用`class`。

```nlang
struct Point {
    int x;
    int y;
}
```

### 值语义

struct是具**深拷贝**语义的**值类型**，贯穿整个语言：

- **声明**（`Point p;`）会把每个字段零初始化。
- **赋值 / 拷贝**（`Point q = p;`）按值复制整个结构体，包括嵌套struct字段，因此源与拷贝相互独立。
- **参数传递 / 返回**：深拷贝进被调方的局部栈帧，再深拷贝回调用方的结果槽位。被调方操作的是自己那份副本。
- **作为class字段**：class持有一份独立的深拷贝。`obj.s = s1`会把`s1`深拷贝进该class的字段槽位。
- **作为数组元素**：`new Point[n]`会急切地为每个元素物化一个全新、独立的struct实例（含嵌套struct字段，递归进行）。把元素读入struct变量（`Point p = arr[i]`）时深拷贝；经下标写入（`arr[i].x = v`、`arr[i] = p`）则存入数组自己的元素。零长度struct数组（`new Point[0]`）合法——`.length`为0，不物化任何元素。

**struct内class引用的浅拷贝**：struct含class类型字段时，struct拷贝会原样复制该class引用（堆索引）。原件与副本指向堆上同一个class对象。这与C#对引用类型struct字段的行为一致。

```nlang
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = new Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // 浅拷贝：b.ref == a.ref（同一对象）
    b.ref.x = 99;        // 修改共享的 Inner 对象
    return a.ref.x;      // 返回 99，不是 10
}
```

### 字段

字段通过成员表达式`p.x`读写；struct没有方法体，因此没有可绑定的`this`。具名初始化（`new Point{x: 1, y: 2}`）设置具名字段；未列出的字段保持零值。完整初始化列表规则见[集合初始化器](collection-initializers.md)。

### struct可以包含

- 基本类型字段（int、float、string等）
- enum字段（按int32存储）
- struct字段（深拷贝，由外层struct持有）
- class字段（引用，浅拷贝）

struct不能包含方法。需要行为请用class。

### 输出

struct没有`toString`，且**永久排除**于字符串强制转换——`"x" + structInstance`是编译错误（struct在NLang中是纯数据类型）。见[类型强制转换](type-casts.md)「Object.toString()协议」。
