> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 6.8 循环

规范性规则见§6.4。

NLang有四种循环形式：`for`、`while`、`do-while`与`foreach`。它们共享`bool`条件规则与`break`/`continue`语义（见[语句](index.md)「条件类型」）。

### `for`

```nlang
for (init; cond; fini) { body }
```

三段语义：

- **初始化段执行一次**（在第一次条件求值之前）；
- **条件段在每轮循环前求值**（包括第一轮）；仅当它为`true`时执行语句体；
- **步进段在每次语句体执行后运行**（包括循环自然结束前的最后一轮）。

`break`立即退出循环（**跳过步进段**）；`continue`跳到下一轮（**先执行步进段**，然后重新求值条件）。在初始化段声明的变量是**函数作用域**——与`foreach`循环变量一样，两者都不受块作用域限制（见下文[Foreach](#foreach)）；语句体内其他声明仅在该体内可见（块作用域）。

不带花括号的单语句循环体（含空语句`;`体）与包住该语句的花括号体完全同语义：体内声明的局部随每轮迭代生效，体内捕获走§7.4的捕获规则。头部声明的变量每次迭代是一个新变量——未捕获时无可观察差异；捕获时各迭代的lambda各自持有当次值，不共享循环结束后的尾值（§7.4）。

```nlang
int sum = 0;
for (int i = 1; i <= 10; i += 1) {
    sum = sum + i;
}
// sum == 55；循环后 `i` 仍在作用域内（函数作用域）
```

```nlang
int count = 0;
for (int j = 0; j < 20; j += 1) {
    if (j == 5) { break; }        // 退出循环（步进段被跳过）
    if (j % 2 == 1) { continue; } // 步进段运行，然后进入下一轮
    count = count + 1;
}
// count == 3（计入j = 0, 2, 4的轮次；j = 5命中break）
```

### `while`

```nlang
while (cond) { body }
```

`while` **先判断后执行**——如果条件从一开始就是`false`，语句体可能从不执行。`break`退出循环；`continue`跳到下一轮（回到条件判断）；在语句体内声明的变量仅在该体内可见（块作用域）。

```nlang
int steps = 0;
while (steps < 3) {
    steps = steps + 1;
}
// steps == 3
```

### `do-while`

```nlang
do { body } while (cond);
```

`do-while` **先执行语句体后判断**——即使条件恒为`false`，语句体也至少执行一次。`break`和`continue`行为同`while`。

```nlang
int n = 0;
do {
    n = n + 1;
} while (false);
// n == 1 —— 语句体执行了一次（do-while至少执行一次）
```

### Foreach

```nlang
foreach (Type var in iterable) { body }
```

迭代`iterable`的元素，把每个元素绑定到`var`供语句体使用。支持的迭代源：

| 迭代源         | 迭代内容                | 元素访问 |
|----------------|-------------------------|----------|
| `T[N]`（数组）  | 元素`arr[0]..arr[N-1]`   | `arr[i]` |
| `List<T>`      | 按插入顺序的元素          | `List<T>.get(i)` |
| `Dict<K,V>`    | **键**按插入顺序（Python风格） | `dict.keys()`键快照后逐个取键 |
| `string`       | **码点**（`char`）        | Unicode转换格式（UTF-8，Unicode Transformation Format）解码推进 |

**源约束**：源表达式必须是数组、`List`、`Dict`或`string`——任何形态均可：lvalue、容器或数组值的调用结果（`List<int[]>`上的`li.get(0)`）、成员访问、`int[n]`、字典下标、裸数组字面量（`[1, 2, 3]`、`[]`——元素类型从循环变量推断）。源只求值一次，所以有副作用的源只运行一次。其他任何源（`int`局部、非容器调用结果）是编译错误："the foreach source must be an array, List, Dict, or string"。

**string源**：循环变量必须是`char`，每轮绑定一个完整码点（多字节字符不会被拆开）——这是string的码点迭代路径，与字节下标`s[i]`相对（见[字符串](../04-types/string.md)「char桥接」）：

```nlang
string s = "héllo";
int n = 0;
foreach (char c in s) {
    n = n + 1;   // 码点计数
}
// n == 5（对比s.length() == 6字节）
```

**循环变量类型**：声明类型必须与元素类型**精确**匹配——元素的基础类型与数组维度必须完全一致。变量可以是数组类型：`grid : List<int[]>`时`foreach (int[] row in grid)`把每个元素绑定为数组。任一维度的不匹配都是编译错误：`grid`是数组时`foreach (int r in grid)`（元素是数组、变量不是）和`nums : List<int>`时`foreach (float x in nums)`（数值拓宽）都以"the foreach variable type does not match the element type"失败。数组类型声明也可作为`for`初始化器（`for (int[] x = arr; ...)`）——初始化生效且语句体可引用该变量。

**引用形态循环变量**：`foreach (int& n in nums)`把循环变量声明为元素存储的引用而非拷贝——通过`n`的写入直接落到容器（语句体内`n = 10`改写的就是该元素）。`&`形态适用于数组与List的标量、string、struct元素；`Dict`与`string`迭代源在编译期拒绝该形态。见[结构体](../04-types/struct.md)中的引用一节。

`break`和`continue`与`for`完全相同。循环变量是**函数作用域**（循环变量无块作用域，与上文`for`形式一致）：

```nlang
List<int> nums = {};
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// 此处 `x` 仍在作用域内（函数作用域）
```

**Dict迭代示例**：

```nlang
Dict<string, int> ages = {};
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**语句体内的修改按迭代源种类有定义**。数组在循环入口一次取定元素计数——自有记录的长度永不改变——语句体内的元素赋值（`arr[i] = x`）没问题（无结构变更）。`List`在每轮循环头部重新探测长度：语句体内`removeAt`使遍历随缩短的存储收尾而不是越过边界，`add`则延长遍历，追加的每个元素恰好被访问一次。`Dict`的foreach走循环入口取定的`keys()`快照，语句体内对字典的`set`/`remove`不会改变遍历形状。

**struct元素被拷贝进循环变量**（值语义）：`foreach (Point p in arr) { p.x = 99; }`不修改`arr`的元素——`p`每轮都是新深拷贝（与C#一致，对值类型元素做foreach也产生拷贝）。引用形态`foreach (Point& p in arr)`绑定元素存储而非拷贝——通过`p`的写入修改`arr`的元素。见[结构体](../04-types/struct.md)。

**null迭代源**在第一次`length()`调用时抛空指针异常（NPE，null pointer exception）——与所有其他class类型调用一致。见[异常](../08-exceptions/index.md)。
