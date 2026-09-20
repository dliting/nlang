# 语句


### 控制流

```nlang
if (cond) { ... }
if (cond) { ... } else { ... }

while (cond) { ... }
do { ... } while (cond);
for (init; cond; fini) { ... }
foreach (Type var in iterable) { ... }

break;
continue;
return;
return expr;
```

**条件类型**：`if`/`while`/`do-while`/`for`/`assert` 的条件必须是
`int`（比较产生 `int`）。string、float、class、struct、array 条件
都是编译错误——VM 的 `OP_JumpIfNot` 读的是单个 int32，非 int 值
（字符串对象句柄、堆索引）没有有意义的真值。请用显式比较代替：
`if (s != "")`、`if (obj != null)`。

### foreach 语句

```nlang
foreach (Type var in iterable) { body }
```

迭代 `iterable` 的元素，把每个元素绑定到 `var` 供循环体使用。支持的
可迭代对象：

| 可迭代对象 | 迭代内容 | 元素访问 |
|----------|----------|----------------|
| `T[N]`（数组） | 元素 `arr[0]..arr[N-1]` | `OP_LoadElement` |
| `List<T>` | 按插入顺序的元素 | `List<T>.get(i)` |
| `Dict<K,V>` | **键**（Python 风格） | 内联 `dict.keys()` 后接 `List<K>.get(i)` |

**源约束**：源表达式必须是数组、`List` 或 `Dict`——左值或返回容器
的调用结果都可以。其他任何源（`int` 局部变量、`string`、非容器调用
结果）都是编译错误："the foreach source must be an array, List, or
Dict"。**数组值**的调用结果（`li.get(0)`，其中 `li : List<int[]>`）
被单独拒绝——"the foreach source is an array value; assign it to a
local first"——因为该值伪装成自己的元素类型；请迭代容器本身，或先把
结果绑定到带类型的局部变量。

**循环变量类型**：声明类型必须与元素类型**完全**一致——底层字段与
数组性都要相同。变量可以是数组类型：`grid : List<int[]>` 时
`foreach (int[] row in grid)` 把每个元素绑定为数组。任一维度不匹配
都是编译错误：`foreach (int r in grid)`（元素是数组而变量不是）与
`nums : List<int>` 时的 `foreach (float x in nums)`（数值加宽）都以
"the foreach variable type does not match the element type" 失败。
数组类型声明也可作 `for` 初始化器（`for (int[] x = arr; ...)`）——
初始化器生效，循环体可以引用该变量。

`break` 与 `continue` 与 `for` 中完全一致。循环变量是**函数作用域**
的（NLang 没有块作用域，与 `for` 一致）：

```nlang
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// `x` 在此处仍在作用域内（函数作用域）
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

**迭代期间改动是未定义行为**。元素数在循环入口缓存（List/Dict 用
`n = iterable.length()`，数组用 `n = arr.length`）。循环体内的结构
性改动（`List.add`/`removeAt`、`Dict.set`/`remove`）可能导致：越界
访问、元素跳过/重复、`keys()` 快照过期。数组 foreach 体内的元素
赋值（`arr[i] = x`）没有问题（非结构性改动）。

**struct 元素拷贝进循环变量**（值语义）：
`foreach (Point p in arr) { p.x = 99; }` 不会修改 `arr` 的元素——
`p` 是每轮迭代的新深拷贝（与 C# 一致：对值类型元素的 foreach 同样
得到副本）。

**null 可迭代对象**在第一次 `length()` 调用时抛 NPE
（与所有其他 class 类型调用一致）。

**含值 0 的 `List<int>`**：由于 `OP_Box` 优化（字面量 `0` 按
null 哨兵对待），对含字面量零元素的 `List<int>` 做 foreach 会抛
`unbox on null/invalid reference`。这是装箱层的限制——规避方法是
避免把 0 作为列表元素值。

### 复合赋值

```nlang
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

`x = x op y` 的读-改-写简写。支持的左值：局部变量、class 字段
（`this.f += y`）、struct 字段（`pt.x += y`）。左值**只求值一次**
（因此 `obj.something() += 1` 不会二次调用 `something()`）。

不支持：下标左值（`arr[i] += 1`）。字节码栈帧布局没有足够的临时
槽位来保证下标读-改-写的单次求值。请用显式形式
`arr[i] = arr[i] + 1`。

### assert 语句

```nlang
assert(condition);
```

求值 `condition`。为假时抛出 `AssertionException`，可被 `try/catch`
块捕获。未捕获则以退出码 1 终止程序。仅单实参形式
（尚无消息重载）。

### 异常处理

NLang 支持结构化异常处理，采用 Java/C# 风格的类层次。所有异常都是
`Exception` 或其子类的实例。

**内建异常类：**

| 类 | 父类 | 抛出场景 |
|-------|-----------|-----------|
| `Exception` | `Object` | 用户 `throw` / Dict 键未找到 |
| `NullPointerException` | `Exception` | null 引用访问 |
| `DivByZeroException` | `Exception` | 整数除法/取模除零 |
| `IndexOutOfBoundsException` | `Exception` | 数组/列表索引越界 |
| `AssertionException` | `Exception` | `assert(false)` |

**try/catch：**

```nlang
try {
    // 可能抛错的代码
} catch (DivByZeroException e) {
    // 处理除零
} catch (Exception e) {
    // 处理其他一切异常
}
```

- 支持多个 `catch` 子句，按声明顺序匹配。
- 执行第一个匹配的 catch 子句；其余跳过。
- `catch (Exception e)` 捕获所有异常（Exception 是基类）。
- catch 变量 `e` 是 catch 体内的普通局部变量。

**throw：**

```nlang
throw new Exception("error message");   // 抛出新异常
throw;                                   // 重抛当前异常（仅在 catch 内）
```

- `throw expr`——表达式必须求值为 Exception 子类实例。抛非 Exception
  值（如 `throw 42`）是编译错误。
- `throw;`（重抛）只在词法上位于 `catch` 体内才有效。在 catch 块外
  使用是编译错误。

**用户自定义异常子类：**

```nlang
class MyException : Exception {
    int code;
    public int MyException(string msg) {
        super(msg);          // 转发到 Exception(message) 构造函数
        this.code = 42;
        return 0;
    }
}
```

用户类可以继承 `Exception` 以携带额外字段。没有用户构造函数时使用
默认构造函数，继承的字段零初始化；用户构造函数通常经 `super(msg)`
转发消息（见下文「super()——构造函数链」）。

**异常字段：**

异常实例暴露两个可读写的字段：

- `message`（string）——异常消息。由构造函数
  （`new Exception("msg")`）或 VM 报错点设置。用户代码可写。
- `backtrace`（List&lt;string&gt;）——VM 抛出的异常在抛出时刻的调用
  栈快照（`funcName.n:line` 条目，最内层在前）。用户构造的异常以空
  backtrace 起步。

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();       // > 0 —— VM 设置了消息
    int frames = e.backtrace.length(); // >= 1 —— VM 快照了调用栈
}

// 用户子类继承两个字段；自有字段排在其后。
class MyException : Exception {
    int code;
}
MyException e = new MyException();
e.message = "custom";   // 可写
e.code = 42;
```

**VM 错误可捕获：**

运行期错误（NPE、除零、数组/列表索引越界、断言
失败）抛出对应的 Exception 子类，可被 `try/catch` 捕获：

```nlang
try {
    int x = 0;
    int y = 1 / x;           // 抛出 DivByZeroException
} catch (DivByZeroException e) {
    // 已捕获
}

try {
    List<int> lst = new List<int>();
    return lst.get(999);      // 抛出 IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // 已捕获
}
```

**未捕获异常**沿调用栈向上传播。找不到处理器时，程序以退出码 1
终止。

**finally：**

```nlang
try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // 总是运行
}
```

`finally` 具备完整的 Java 语义——控制流经**任何**一条路径离开 try
区域时，finally 体都会运行：

- try 体正常完成（包括从底部落出）
- 某个 catch 子句完成（无论是否匹配）
- 异常从中穿过（finally 处理器运行自己那份体拷贝，然后重抛原异常；
  包括从 catch 体内抛出的异常）
- `break` / `continue` 传出该区域（跳转前运行 finally 体的内联拷贝，
  嵌套 try 时由内向外）
- `return`（先求值返回表达式，再运行 finally 体，然后函数返回）

嵌套：内层 finally 体先于外层运行；其后，外层的 catch（若有）才
看到异常。每条控制流路径上每个 finally 体恰好执行一次——正常路径
与异常路径的拷贝是不相交的代码区域。

不带任何 catch 子句的 `try { } finally { }` 合法（finally 的入口是
唯一的处理器）。

**限制**：`break`、`continue`、`return`、`throw` *不得出现在
finally 体内*（编译错误）。finally 体不得吞掉在途的控制流或
异常。

**super()——构造函数链：**

```nlang
class Base {
    public int v;
    public int Base(int x) { this.v = x; return 0; }
}
class Kid : Base {
    public int Kid(int x) {
        super(x * 2);        // 调用 Base(int)
        return 0;
    }
}
```

- `super(args);` 在同一个 `this` 对象上调用**直接父类的构造函数**。
  仅在有父类的类的构造函数内有效（`Object` 没有父类）。
- 可出现在构造函数的**任何语句位置**（不限于第一条语句）。
- 实参个数必须与父构造函数的形参个数一致。对内建 Exception 家族的父类，
  构造函数恰好接受一个 `message` 实参。
- 不支持具名实参（`super(x = 1)`，编译错误）。
- 对没有构造函数的父类调用无参 `super()` 是合法的无操作；此时传
  实参是编译错误。

### const 局部变量

```nlang
const int X = 5;
const string Greeting = "hello";
```

标记 `const` 的局部变量必须在声明时初始化，此后不能再赋值或复合
赋值。只支持**局部** const；class/struct 字段 const 不支持。

**const 是浅层的（Java `final` 风格）**：`const` 阻止对*名字*的
重绑定，但不冻结被引用对象的状态。经 const 局部变量做成员改动是
允许的：

```nlang
const Foo f = new Foo();
f.x = 5;            // OK —— f 本身未被重新赋值
f = new Foo();      // 错误 —— 不能对 const 局部变量重新赋值
const Point p = q;
p.x = 5;            // OK —— 只有 p 的绑定是 const
```

对 class 字段与数组元素而言：`const` 引用仍允许经它写入。深层的/
不可变风格的 const 不受支持，将来可能重新审视。

### switch

```nlang
switch (value) {
    case 1, 2: ...
    case 3: ...
    default: ...
}
```

**判别式家族。**switch 判别式可以是 `int`、`float`、`string` 或
枚举类型的值（枚举按其 int 值比较）。class、struct、array 与
`null` 判别式在编译期被拒绝。

**带类型的相等。**比较使用判别式所属家族的相等运算符：int 与枚举
精确比较；float 在 IEEE 语义下比较（`-0.0 == 0.0` 为真，NaN 不等于
任何值，包括自身）；string 按内容比较，不按恒等。case 标签必须属于
与判别式相同的家族——没有跨家族转换（int 判别式上写 `case "1"` 是
编译错误）。`null` 不是合法的 case 标签。标签可以是计算表达式
（如 `case f(x):`）——它们在运行期按子句顺序求值。

**多值标签。**一个 case 子句可以列出多个标签（`case 1, 2:`）；任一
标签命中即运行子句体。

**禁止穿透。**每个 case 体以一条隐式跳出 switch 的跳转收尾——即使
没有显式 `break` 语句，执行也不会级联进下一个 case 体。这与 Java/C#
语义一致，与 C/C++ 不同。`break` 关键字只在需要从多语句 case 体内
提前退出时才需要。case 体内的 `break` 总是绑定到 switch 本身，绝不
绑定到外层循环。

**重复标签。**同一 switch 内值相同的常量标签（数字/字符串字面量与
枚举成员）——跨子句或在单个多值子句内——在编译期被拒绝
（`case 1:` 加上 `Red = 0` 时的 `case Color.Red:` 即重复）。重复的
非常量标签（两个都返回 1 的调用）允许；首个命中者获胜。

### Null 检查

对 null class 引用访问字段或方法抛出 `NullPointerException`，
可被 `try/catch` 块捕获。未捕获时程序以退出码 1 终止。
