# 集合初始化器


NLang 支持 C 风格的集合字面量，用于数组、列表、字典以及聚合（struct/class）
初始化。两种语法形式：

**裸括号形式 `[...]`** —— 仅当左值或赋值目标能让编译器推断出集合类型时才
允许。适用于数组（`T[]`）和 `List<T>`：

```nlang
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [new Point{x:1, y:2}, new Point{x:3, y:4}];
```

**显式形式 `new Type{...}`** —— 适用于任意表达式位置（函数实参、返回值、独立
表达式）。字典、struct、class 初始化必须用它，因为裸 `{...}` 会与块语句语法
冲突（`{...}` 包围的语句组）：

```nlang
Dict<string, int> d = new Dict<string, int>{"a":1, "b":2};
Point p = new Point{x:1, y:2};
List<int> lst = new List<int>{1, 2, 3};
return new Point{x:0, y:0};
foo(new Point{x:1, y:2}, new Point{x:3, y:4});
```

**`{...}` 内的条目形式：**

- `字符串字面量 : 表达式` —— 字典条目（string 键）
- `标识符 : 表达式` —— struct/class 字段（例如 `x:1, y:2`）
- `表达式`（无键）—— 列表元素（仅当 Type 是 `List<T>` 时合法）

**字典键必须是 string 字面量**：集合初始化器把每个键按字符串常量发射并按
K 的标签装箱——这只对 `K = string` 正确。非 string 键的 `Dict<K,V>` 带非空
初始化器是编译错误（`new Dict<int, int>{"1": 2}` 报
`the Dict collection initializer requires string keys; use set() with
an explicit 'Int32' key`）；空初始化器 `new Dict<int, int>{}` 合法，随后
用 `set()` 逐条填入。

**类型消歧**：编译器用左值变量（或 `new Type{...}` 中的显式 `Type`）来选择类
型：

| 目标类型              | 形式              | 条目种类                          |
|------------------------|-------------------|-----------------------------------|
| `T[]`（数组）           | `[...]`            | 仅值                              |
| `List<T>`               | `[...]` 或 `new List<T>{...}` | 仅值 |
| `Dict<K,V>`              | `new Dict<K,V>{...}` | `key : value`（string 键） |
| struct                   | `new StructName{...}` | `field : value`（标识符键） |
| class                    | `new ClassName{...}`  | `field : value`（标识符键） |

**class 初始化要求**：类必须有无参构造函数（显式或隐式）。codegen 把
`new C{f1:v1, ...}` 降低为 `new C()` 后接逐字段 `OP_StoreField` 赋值。

**递归嵌套**：初始化列表可包含其他初始化列表。嵌套泛型元素类型
（`List<List<int>>`、`Dict<K, List<V>>`）受支持（见
[内建泛型类](builtin-generic-classes.md) `List<T>` 下的嵌套泛型）。

**空集合**：裸 `[]` 不受支持（词法器把 `[]` 匹配为单个 token，供数组类型后缀
语法使用）。改用显式空形式：`new List<T>{}`、`new Dict<K,V>{}`，或数组用
`new int[0]`（见 [数组](array.md)）。

**函数实参消歧**：作为函数实参的裸 `[...]` 目前不受支持——因为没有重载解析，
编译器无法推断目标类型，所以会产生编译错误。函数实参请用显式
`new Type{...}` 形式。

**初始化期间变更是未定义行为。** 条目从左到右求值并按序赋值；在条目表达式内
读部分构造的集合（例如 `[1, foo(arr)]` 中 `foo` 读 `arr`）是未定义行为。抛异
常的条目会留下部分构造的集合。
