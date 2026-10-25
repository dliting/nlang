# 集合初始化器


NLang支持集合字面量，用于数组、列表、字典以及聚合（struct/class）初始化。三种语法形式：

**裸方括号形式`[...]`** —— 当目标类型能让编译器推断出集合时允许（声明初始化、赋值、返回值、`foreach`源、位置与命名实参、容器元素）。适用于数组（`T[]`）和`List<T>`：

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [Point{x:1, y:2}, Point{x:3, y:4}];
```

**裸花括号形式`{...}`** —— 由带类型目标决定读法：键值条目初始化字典与struct/class聚合，位置条目初始化列表。适用推断位与`[...]`相同，`foreach`源除外——该位置的目标恒为循环变量的数组类型，花括号形式会被拒绝：

```nlang
Dict<string, int> d = {"a":1, "b":2};
List<int> lst = {1, 2, 3};
take({7, 0});
count(items = {1, 2});
List<int> mk() { return {1, 2}; }
```

**锚定形式`Type{...}`** —— 具名class/struct头锚定字面量，因此适用于任意表达式位置（函数实参、返回值、独立表达式）：

```nlang
Point p = Point{x:1, y:2};
return Point{x:0, y:0};
foo(Point{x:1, y:2}, Point{x:3, y:4});
```

泛型实例化头不能锚定——`List<int>{60}`无法解析；改为给字面量一个带类型目标（`List<int> l = {60};`）。

**`{...}`内的条目形式：**

- `字符串字面量 : 表达式` —— 字典条目（string键）
- `标识符 : 表达式` —— struct/class字段（例如`x:1, y:2`）
- `表达式`（无键）—— 列表元素，或struct位置条目：struct按字段声明顺序填充，`Point{3, 7}`等价于`Point{x:3, y:7}`，可以只给前几个字段（其余保持默认值）。class目标不接受位置条目——`P{3, 7}`报`class initializer "P{...}" requires field:value entries; the positional form is only valid for struct/array targets`。

**字典键必须是string字面量**：初始化器只支持string键——这只对`K = string`正确。非string键的`Dict<K,V>`带非空初始化器是编译错误（`Dict<int, int> d = {"1": 2}`报`the Dict collection initializer requires string keys; use set() with an explicit 'Int32' key`）；空初始化器`Dict<int, int> d = {}`合法，随后用`set()`逐条填入。

**类型消歧**：编译器用带类型目标（声明的变量、形参类型、返回类型）或锚定类型名来选择种类：

| 目标类型              | 形式              | 条目种类                          |
|------------------------|-------------------|-----------------------------------|
| `T[]`（数组）           | `[...]`            | 仅值                              |
| `List<T>`               | `[...]`或`{...}`   | 仅值                              |
| `Dict<K,V>`              | `{...}`            | `key : value`（string键） |
| struct                   | `{...}`或`StructName{...}` | `field : value`（标识符键）或位置值（按字段声明序） |
| class                    | `{...}`或`ClassName{...}`  | `field : value`（标识符键） |

**class初始化要求**：类必须有无参构造函数（显式或隐式）。`C{f1:v1, ...}`等价于先无参构造、再逐字段赋值。

**递归嵌套**：初始化列表可包含其他初始化列表。内层可用方括号形式（`List<List<int>> ll = [[1,2],[3,4]];`），也可在花括号字面量的值位置直接嵌裸花括号（`List<List<int>> m = {{1,2},{3,4}};`）。带键条目仍要求表达式值——struct/class字面量里的`{items: {1,2}}`是语法错误，写`{items: [1,2]}`。嵌套泛型元素类型（`List<List<int>>`、`Dict<K, List<V>>`）受支持（见[内建泛型类](builtin-generic-classes.md) `List<T>`下的嵌套泛型）。

**空集合**：空字面量对`List<T>`与`Dict<K,V>`目标写作`{}`——`List<int> l = {};`、`Dict<K,V> d = {};`。数组目标无花括号形式（`int[] a = {};`被拒绝——数组初始化用方括号形式）；空数组写作`int[] a = [];`（见[数组](array.md)）。空方括号字面量同样可作`foreach`源与函数实参。

**函数实参**：位置实参从形参类型推断目标——位置条目适用于struct与`List<T>`形参（struct `Point`形参可写`take({7, 0})`）；数组形参用方括号形式（`take([7, 0])`）；class形参须用`字段:值`条目（见上表）。命名实参同样推断（`List<int>`形参可写`count_of(items = {1, 2, 3})`）。

**初始化期间变更是未定义行为。**条目从左到右求值并按序赋值；在条目表达式内读部分构造的集合（例如`[1, foo(arr)]`中`foo`读`arr`）是未定义行为。抛异常的条目会留下部分构造的集合。
