# 内建泛型类


### `List<T>`

`List<T>`是可增长、有序、可按索引访问的集合。它是**内建泛型类**——编译器只识别`List`与`Dict`；用户自定义的`class Foo<T>`不支持。

```nlang
List<int> nums = new List<int>();
nums.add(1);
nums.add(2);
nums.add(3);
int sum = nums.get(0) + nums.get(1) + nums.get(2);    // 6
int n = nums.length();                                 // 3

List<string> names = new List<string>();
names.add("alice");
names.add("bob");
int total = (names.get(0) + names.get(1)).length();    // 8
```

**方法**（T为元素类型）：

| 方法              | 签名          | 返回 | 说明                              |
|---------------------|--------------------|---------|------------------------------------|
| `add`               | `void add(T item)` | —       | 追加到末尾                      |
| `get`               | `T get(int idx)`   | T       | 按索引读取；越界抛错       |
| `set`               | `void set(int i, T)` | —     | 覆写元素                  |
| `length`            | `int length()`     | int     | 当前元素数              |
| `removeAt`          | `void removeAt(int i)` | —   | 擦除；后继元素前移  |
| `indexOf`           | `int indexOf(T item)` | int | `item`的首个索引，无则 -1       |
| `contains`          | `bool contains(T item)` | bool | 存在为true，否则false               |
| `clear`             | `void clear()`     | —       | 移除全部元素                |

**类型检查**：编译器把`List<int>`、`List<string>`、`List<Point>`等识别为彼此不同的静态类型。实参类型按替换后的签名检查——`nums : List<int>`时`nums.add("wrong")`是编译错误。

**擦除运行期模型**：`List<int>`与`List<Point>`在运行期共享同一个后备class。元素统一以堆索引形式存入侧表；标量基本类型元素（全部12个）在调用点经`OP_Box`装箱。垃圾回收（GC，garbage collection）把列表元素作为附加根追踪。

**数组类型实参**：`T`可以是数组类型——`List<int[]>`把`int[]`值作为裸的、GC可追踪的句柄存储；上文的基本类型装箱规则不适用于数组类型的元素。经`get`/下标取出的元素保留其数组类型，`foreach (int[] row in grid)`可直接迭代它们。`indexOf`/
`contains`按句柄恒等比较。锯齿实参（`List<int[][]>`）与其他锯齿声明一样被拒绝。

**null List引用**：未赋值`new List<T>()`的`List<T>`字段或变量持有null。对null调用任何方法抛出`null reference in CallMethod`——与其他class引用相同的空指针异常（NPE，null pointer exception）语义。

**集合初始化器**：支持`[1, 2, 3]`字面量语法（数组与`List<T>`的裸方括号形式）。见[集合初始化器](collection-initializers.md)。

**`foreach`**：支持`foreach (Type var in iterable)`构造。见[foreach语句](foreach.md)。

**嵌套泛型**（`List<List<int>>`）：支持。词法分析器跟踪类型实参的嵌套深度（内建泛型名`List`/`Dict`之后紧跟的`<`开一层，每个`>`关一层），深度为正时把`>>`拆成两个`'>'`记号（C#式扫描器拆分），因此`List<List<int>>`与`Dict<string, List<int>>`都能解析。泛型上下文之外的`>>`仍是单个记号（不拆分）——右移没有对应的语法规则，所以`x >> 2`是编译错误（移位运算符本身未实现）。限制：与遮蔽了类型名的变量做比较（`List < 3`，名字与`<`之间只有空白/注释）会被误读为泛型开启。

**跨模块**：容器泛型签名可以跨越`.ncu`导入边界——被导入函数递归携带真实的形参与返回类型（`List<int[]>`、`Dict`实例化、嵌套数组），调用点类型检查与同模块调用一致。函数（`Func`）签名仍在描述符文法之外（见「已知限制」）。

### `Dict<K,V>`

`Dict<K,V>`是把`K`类型的键映射到`V`类型值的关联数组。与`List<T>`一样，它是**内建泛型类**——编译器只识别`List`与`Dict`；用户自定义泛型不支持。

```nlang
Dict<string,int> scores = new Dict<string,int>();
scores.set("alice", 90);
scores.set("bob",   85);
int a = scores.get("alice");          // 90
bool hasBob = scores.containsKey("bob"); // true
int n = scores.count();                 // 2

Dict<int,int> squares = new Dict<int,int>();
squares.set(3, 9);
squares.set(4, 16);
squares.set(3, 99);                     // 覆写 9 → 99
int v = squares.get(3);                 // 99
int removed = squares.remove(4);        // 1
```

**方法**（K为键类型，V为值类型）：

| 方法           | 签名                  | 返回 | 说明                                          |
|------------------|----------------------------|---------|------------------------------------------------|
| `set`            | `void set(K key, V value)` | —       | 插入或替换（无重复键错误）     |
| `get`            | `V get(K key)`             | V       | 查找；键不存在时**抛错**               |
| `containsKey`    | `bool containsKey(K key)`  | bool    | 存在为true，否则false                      |
| `remove`         | `int remove(K key)`        | int     | 删除为1，键未找到为0               |
| `clear`          | `void clear()`             | —       | 移除全部条目                             |
| `count`          | `int count()`              | int     | 当前条目数                            |

**类型检查**：编译器把`Dict<int,int>`、`Dict<string,Point>`等识别为彼此不同的静态类型。实参类型按替换后的签名检查——`d : Dict<string,int>`时`d.set("x", "y")`是编译错误。

**擦除运行期模型**：`Dict<K,V>`在所有实例化之间共享同一个后备class。条目以`(K 堆索引, V 堆索引)`对的形式存入侧表；基本类型键/值在调用点经`OP_Box`装箱。GC把每条目的K与V作为附加根追踪。

**键相等**是kind感知的：
- 标量基本类型键（装箱整型家族、`float`/`double`、`bool`、`char`）：比较值位（IEEE 754——`NaN != NaN`，已记录在案的行为）。
- `string`键：比较字符串内容（值相等）。
- `class` / `struct`键：比较堆索引（恒等），与Java的`IdentityHashMap`、C# 默认的`object.Equals`一致。用户的`equals`覆写**不参与比较**——基于覆写`equals`的键相等语义不受支持。
- 数组键（`Dict<int[], V>`）：按句柄恒等比较——两个内容相同但独立的`int[2]`数组是不同的键。

**null Dict引用**：未赋值`new Dict<K,V>()`的`Dict<K,V>`字段或变量持有null。对null调用任何方法抛出`NLang VM: Dict <method> on null instance`。

**线性扫描查找（当前限制）**：每次`set`/`get`/`containsKey`/
`remove`都对条目向量做O(n)扫描。对典型的小脚本可以接受；不提供O(1)哈希表查找。

**对键的`foreach`**：`foreach (K k in dict) { ... }`以Python/JavaScript风格迭代dict的键。循环体内调用`dict.get(k)`访问值。实现：编译器先内联一次`dict.keys()`调用物化一个新`List<K>`，再对该列表迭代。见[foreach语句](foreach.md)。

**`Dict.keys()`**：返回装好全部键的新`List<K>`（无定义顺序）。即使不用`foreach`也有用：给键做快照以便枚举、经`contains`做集合式成员检查等。返回的`List<K>`是*副本*——此后对源dict的`set`/`remove`不影响它。

**`values()`**：不提供。请迭代键并调用`get`取值。

### 下标语法糖——`li[i]` / `d[k]`

两种容器都支持下标语法，纯粹是`get` / `set`内建函数之上的语法糖（零新指令）：

```nlang
List<int> li = new List<int>();
li.add(2);
li.add(5);
int v = li[1];          // == li.get(1)      -> 5
li[0] = 9;              // == li.set(0, 9)

Dict<string, int> d = new Dict<string, int>();
d["a"] = 3;             // == d.set("a", 3)
int x = d["a"];         // == d.get("a")     -> 3
```

越界读写与缺失的dict键的抛错行为与方法形式完全一致（IndexOutOfBoundsException家族）。

由于编译器会从容器的类型剥出元素类型T/V，下标能与语言其余部分自由组合：

```nlang
List<List<int>> m = ...;
m[0][1] = 47;           // 链式下标写

List<Point> pts = ...;
pts[0].x = 9;           // 经下标接收者的成员写
int s = pts[0].x + pts[0].y;

List<int>[] arr = new List<int>[2];   // 泛型实例化的数组
arr[0] = new List<int>();
arr[0][1];              // 穿过数组元素的容器下标
```

复合下标赋值（`li[0] += 1`）有意不支持（与数组同一策略）；请写`li[0] = li[0] + 1`。
