# Foreach


```nlang
foreach (Type var in iterable) { body }
```

迭代 `iterable` 的元素，把每个元素绑定到 `var` 供语句体使用。支持的迭代源：

| 迭代源         | 迭代内容                | 元素访问 |
|----------------|-------------------------|----------|
| `T[N]`（数组）  | 元素 `arr[0]..arr[N-1]`   | `OP_LoadElement` |
| `List<T>`      | 按插入顺序的元素          | `List<T>.get(i)` |
| `Dict<K,V>`    | **键**（Python 风格）      | 内联 `dict.keys()` 然后 `List<K>.get(i)` |

**源约束**：源表达式必须是数组、`List`、或 `Dict`——任何形态均可：lvalue、
容器或数组值的调用结果（`List<int[]>` 上的 `li.get(0)`）、成员访问、
`new int[n]`、字典下标。源只求值一次（绑定到一个隐藏迭代局部），所以有副作用
的源只运行一次。其他任何源（`int` 局部、`string`、非容器调用结果）是编译错
误："the foreach source must be an array, List, or Dict"。

**循环变量类型**：声明类型必须与元素类型**精确**匹配——相同底层字段与相同数
组性。变量可以是数组类型：`grid : List<int[]>` 时 `foreach (int[] row in
grid)` 把每个元素绑定为数组。任一维度的不匹配都是编译错误：`grid` 是数组时
`foreach (int r in grid)`（元素是数组、变量不是）和 `nums : List<int>` 时
`foreach (float x in nums)`（数值拓宽）都以 "the foreach variable type does
not match the element type" 失败。数组类型声明也可作为 `for` 初始化器
（`for (int[] x = arr; ...)`）——初始化生效且语句体可引用该变量。

`break` 和 `continue` 与 `for` 完全相同。循环变量是**函数作用域**（循环变量
无块作用域，与 `for` 一致——见 [循环](loops.md)）：

```nlang
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// 此处 `x` 仍在作用域内（函数作用域）
```

**Dict 迭代示例**：

```nlang
Dict<string, int> ages = new Dict<string, int>();
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**变更是未定义行为**。元素计数在循环入口被缓存（List/Dict 用
`n = iterable.length()`，Array 用 `n = arr.length`）。语句体内的结构修改
（`List.add`/`removeAt`、`Dict.set`/`remove`）可能导致：越界访问、跳过/重复
元素、或过期的 `keys()` 快照。对 Array foreach 语句体内的元素赋值
（`arr[i] = x`）没问题（无结构变更）。

**struct 元素被拷贝进循环变量**（值语义）：`foreach (Point p in arr)
{ p.x = 99; }` 不修改 `arr` 的元素——`p` 每轮都是新深拷贝（与 C# 一致，对
值类型元素做 foreach 也产生拷贝）。见 [结构体](struct.md)。

**null 迭代源**在第一次 `length()` 调用时抛 NPE（与所有其他 class 类型调用
一致）。见 [异常](exception.md)。

**含值 0 的 `List<int>`**：由于 `OP_Box` 优化（字面 `0` 被视为 null 哨兵），
对含字面零元素的 `List<int>` 做 `foreach` 会抛 `unbox on null/invalid
reference`。这是装箱层的限制——通过避免用 0 作为列表元素值来规避。
