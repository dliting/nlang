# 集合初始化器


NLang支持集合字面量，用于数组、列表、字典以及聚合（struct/class）初始化。三种语法形式：

**裸方括号形式`[...]`** —— 当目标类型能让编译器推断出集合时允许（声明初始化、赋值、返回值、`foreach`源、位置与命名实参、容器元素）；无目标位置（方法接收者、下标基）按条目推断元素类型（空字面量与异类条目仍需带类型目标）。适用于数组（`T[]`）和`List<T>`：

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [Point{x:1, y:2}, Point{x:3, y:4}];
```

**裸花括号形式`{...}`** —— 由带类型目标决定读法：键值条目初始化字典与struct/class聚合，位置条目初始化列表。适用推断位与`[...]`相同；`foreach`源位置按循环变量类型推断`List<T>`：

```nlang
Dict<string, int> d = {"a":1, "b":2};
List<int> lst = {1, 2, 3};
take({7, 0});
count(items = {1, 2});
List<int> mk() { return {1, 2}; }
```

**锚定形式`Type{...}`** —— 具名class/struct或内建泛型头锚定字面量，因此适用于任意表达式位置（函数实参、返回值、独立表达式）：

```nlang
Point p = Point{x:1, y:2};
return Point{x:0, y:0};
foo(Point{x:1, y:2}, Point{x:3, y:4});
```

泛型实例化头同样可锚定（`List<int>{60}`、`Dict<string, int>{"a": 1}`），适用于任意表达式位置；锚定形式限内建泛型名（`List`、`Dict`、`Channel`、`Future`）。

**`{...}`内的条目形式：**

- `表达式 : 表达式` —— 带键条目。`Dict<K,V>`目标中键是完整表达式，按K检查（见下文「字典键是表达式」）；struct/class目标中键必须是标识符，即字段名（例如`x:1, y:2`）——那里的标识符是字段名而非变量
- `表达式`（无键）—— 列表元素，或struct位置条目：struct按字段声明顺序填充，`Point{3, 7}`等价于`Point{x:3, y:7}`，可以只给前几个字段（其余保持默认值）。class目标不接受位置条目——`P{3, 7}`报`class initializer "P{...}" requires field:value entries; the positional form is only valid for struct/array targets`。

**条目形式必须与目标种类匹配**：不匹配的条目是编译错误——`List<T>`初始化器里的键值条目（`List<int> l = {x: 1};`报`a List initializer takes positional entries; the keyed form is only valid for Dict, struct, and class initializers`）、数组初始化器里的键值条目（`int[] arr = [1: 2];`报`an array initializer takes positional entries; the keyed form is only valid for Dict, struct, and class initializers`）、`Dict<K,V>`初始化器里的无键条目（`Dict<string, int> d = {1, 2};`报`a Dict initializer requires a key in every entry; positional entries are only valid for List, struct, and array initializers`）、struct/class初始化器里的表达式键（`S s = {"a": 1};`报`struct initializer "S{...}" keys are field names; an expression key is only valid for Dict initializers`，`C c = {f(): 1};`报`class initializer "C{...}" requires field:value entries; the positional form is only valid for struct/array targets`）。

**字典键是表达式**：`Dict<K,V>`初始化器的每个条目都带一个键表达式，按与值相同的转换规则对照声明的键类型K检查——字面量、变量、调用、成员访问与复合表达式都可以（`Dict<int, string> d = {1: "one", n: "ten"};`、`Dict<string, int> d = {k: 7, c.name: 8, a + "y": 3};`）。转换矩阵不放行的键类型是编译错误（`Dict<int, int> d = {"a": 1};`报`Incompatible type "a".`）。键先于值求值、条目从左到右；重复的键覆盖先前的条目，与运行期`set()`语义相同。

**类型消歧**：编译器用带类型目标（声明的变量、形参类型、返回类型）或锚定类型名来选择种类：

| 目标类型              | 形式              | 条目种类                          |
|------------------------|-------------------|-----------------------------------|
| `T[]`（数组）           | `[...]`            | 仅值                              |
| `List<T>`               | `[...]`或`{...}`   | 仅值                              |
| `Dict<K,V>`              | `{...}`            | `key : value`（按K检查的键表达式） |
| struct                   | `{...}`或`StructName{...}` | `field : value`（标识符键）或位置值（按字段声明序） |
| class                    | `{...}`或`ClassName{...}`  | `field : value`（标识符键） |

**class初始化要求**：类必须有无参构造函数（显式或隐式）。codegen把`C{f1:v1, ...}`降低为无参构造后接逐字段`OP_StoreField`赋值。

**递归嵌套**：初始化列表可包含其他初始化列表。内层可用方括号形式（`List<List<int>> ll = [[1,2],[3,4]];`），也可在花括号字面量的值位置直接嵌裸花括号（`List<List<int>> m = {{1,2},{3,4}};`）。带键条目的值位置同样接受裸花括号（`Dict<string, List<int>>`目标写`{"a": {1, 2}}`，`List<int>`字段写`{items: {1, 2}}`）；数组类型字段保持方括号形式（`{items: [1, 2]}`）。嵌套泛型元素类型（`List<List<int>>`、`Dict<K, List<V>>`）受支持（见[内建泛型类](builtin-generic-classes.md) `List<T>`下的嵌套泛型）。

**空集合**：空字面量对`List<T>`与`Dict<K,V>`目标写作`{}`——`List<int> l = {};`、`Dict<K,V> d = {};`。数组目标无花括号形式（`int[] a = {};`被拒绝——数组初始化用方括号形式）；空数组写作`int[] a = [];`（见[数组](array.md)）。空方括号字面量同样可作`foreach`源与函数实参。无目标位置的方括号空字面量无法推断元素类型（`[].length()`被拒绝），仍须带类型目标。

**函数实参**：位置实参从形参类型推断目标——位置条目适用于struct与`List<T>`形参（struct `Point`形参可写`take({7, 0})`）；数组形参用方括号形式（`take([7, 0])`）；class形参须用`字段:值`条目（见上表）。命名实参同样推断（`List<int>`形参可写`count_of(items = {1, 2, 3})`）。

**初始化期间变更是未定义行为。**条目从左到右求值并按序赋值；在条目表达式内读部分构造的集合（例如`[1, foo(arr)]`中`foo`读`arr`）是未定义行为。抛异常的条目会留下部分构造的集合。
