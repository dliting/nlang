# 枚举


`enum` 声明一组具名整数常量。枚举值在运行期是 `int32`。

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

成员可显式赋值，也可自动递增。

### 值语义

枚举是 int32 为底座的**值类型**：

- **赋值 / 传参 / 返回**：拷贝 int32 值。两个变量持有独立值；修改一个绝不
  影响另一个。
- **比较**：按整数值比较——`==`/`!=` 与关系运算符都比较底层 int32
  （见 [运算符](operators.md)）。
- **无对象身份**：枚举值没有堆身份。`Color.Red` 就是 int32 值 `1`（或它被
  赋的值），没有可取哈希或按引用比较的对象。对枚举的 `switch` 按 int 值
  匹配（见 [switch](switch.md)）。
- **在聚合内**：struct/class 中 enum 类型的字段按 int32 存储
  （见 [结构体](struct.md)、[类](class.md)）。

### 枚举方法

```nlang
enum Color {
    Red = 1, Green = 2, Blue = 4;

    public int weight(int base) {
        return this * base;
    }

    public int isPrimary() {
        switch (this) {
            case Color.Red, Color.Green, Color.Blue: return 1;
        }
        return 0;
    }
}
```

方法声明在成员列表之后，以 `;` 分隔。只有孤立结尾 `;` 而无方法的
`enum E { A; }` 也接受（Java 式收尾语法）。每个方法都必须有方法体（抽象
方法被拒绝）。

- **`this` 就是枚举值。**方法体内 `this` 是接收者的 int32 值：可直接参与
  算术与 `switch`（`this * base`、`switch (this)`），没有装箱。方法静态
  分派（`OP_CallMethodDirect`）——枚举没有继承，也没有虚分派。
- **经接收者调用。**`c.weight(3)`、`this.weight(3)`、`Color.Red.weight(3)`
  ——任何枚举值表达式都可以。裸写的 `weight(3)`（无接收者）是编译错误，
  与 class 方法一致。
- **参数：**不支持默认值（`int f(int a, int b = 5)` 被拒绝），也不支持
  `out` 参数。
- **`toString` 保留。**`toString()` 内建函数（值 → 名字）不能被用户方法
  遮蔽。
- **`this.<成员>` 按常量解析。**方法内的 `this.Red` 读的是成员 `Red` 的
  值——不与接收者比较。方便，但容易误读；拿不准时请显式命名接收者。
- **成员/方法同名即冲突**（`enum E { f; int f() {...} }` 被拒绝）。访问
  修饰符遵循 class 方法规则。
- **不支持跨模块枚举。**被导入模块里声明的枚举类型对导入方不可见
  （`.nmod` 格式只序列化枚举名，不序列化声明）——这是模块格式的限制，
  与方法无关。
- 枚举数组：不能对数组本身调用方法——先索引出元素（`a[i].rank()`，不是
  `a.rank()`）。见 [数组](array.md)。

### 输出

`Color.Red.toString()` 返回 `"Red"`（不是 `"0"`）。编译器内嵌每个枚举一张
名字表；VM 用 `OP_Enum_to_str` 按值查成员名。越界的枚举值在运行期抛错。
完整 `toString` 协议见 [类型强制转换](type-casts.md)。
