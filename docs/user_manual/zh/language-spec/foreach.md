# Foreach


```nlang
foreach (Type var in iterable) { body }
```

迭代`iterable`的元素，把每个元素绑定到`var`供语句体使用。支持的迭代源：

| 迭代源         | 迭代内容                | 元素访问 |
|----------------|-------------------------|----------|
| `T[N]`（数组）  | 元素`arr[0]..arr[N-1]`   | `OP_LoadElement` |
| `List<T>`      | 按插入顺序的元素          | `List<T>.get(i)` |
| `Dict<K,V>`    | **键**（Python风格）      | 内联`dict.keys()`然后`List<K>.get(i)` |
| `string`       | **码点**（`char`）        | Unicode转换格式（UTF-8，Unicode Transformation Format）解码推进 |

**源约束**：源表达式必须是数组、`List`、`Dict`或`string`——任何形态均可：lvalue、容器或数组值的调用结果（`List<int[]>`上的`li.get(0)`）、成员访问、`new int[n]`、字典下标。源只求值一次（绑定到一个隐藏迭代局部），所以有副作用的源只运行一次。其他任何源（`int`局部、非容器调用结果）是编译错误："the foreach source must be an array, List, Dict, or string"。

**string源**：循环变量必须是`char`，每轮绑定一个完整码点（多字节字符不会被拆开）——这是string的码点迭代路径，与字节下标`s[i]`相对（见[字符串](string.md)「char桥接」）：

```nlang
string s = "héllo";
int n = 0;
foreach (char c in s) {
    n = n + 1;   // 码点计数
}
// n == 5（对比 s.length() == 6 字节）
```

**循环变量类型**：声明类型必须与元素类型**精确**匹配——元素的基础类型与数组维度必须完全一致。变量可以是数组类型：`grid : List<int[]>`时`foreach (int[] row in grid)`把每个元素绑定为数组。任一维度的不匹配都是编译错误：`grid`是数组时`foreach (int r in grid)`（元素是数组、变量不是）和`nums : List<int>`时`foreach (float x in nums)`（数值拓宽）都以 "the foreach variable type does
not match the element type" 失败。数组类型声明也可作为`for`初始化器（`for (int[] x = arr; ...)`）——初始化生效且语句体可引用该变量。

`break`和`continue`与`for`完全相同。循环变量是**函数作用域**（循环变量无块作用域，与`for`一致——见[循环](loops.md)）：

```nlang
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// 此处 `x` 仍在作用域内（函数作用域）
```

**Dict迭代示例**：

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

**变更是未定义行为**。元素计数在循环入口被缓存（List/Dict用`n = iterable.length()`，Array用`n = arr.length`）。语句体内的结构修改（`List.add`/`removeAt`、`Dict.set`/`remove`）可能导致：越界访问、跳过/重复元素、或过期的`keys()`快照。对Array foreach语句体内的元素赋值（`arr[i] = x`）没问题（无结构变更）。

**struct元素被拷贝进循环变量**（值语义）：`foreach (Point p in arr)
{ p.x = 99; }`不修改`arr`的元素——`p`每轮都是新深拷贝（与C#一致，对值类型元素做foreach也产生拷贝）。见[结构体](struct.md)。

**null迭代源**在第一次`length()`调用时抛空指针异常（NPE，null pointer
exception）——与所有其他class类型调用一致。见[异常](exception.md)。
