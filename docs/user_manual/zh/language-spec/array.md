# 数组


数组是定长、可按索引寻址的序列。语法为 `T[]`。

```nlang
int[] a = new int[3];    // 堆上定长数组，元素零初始化
int[] b = [1, 2, 3];     // 裸初始化器（见 集合初始化器）
int[] c = new int[0];    // 空数组
```

### 引用语义

数组是**引用类型**：数组在堆上，赋值与参数传递拷贝数组**引用**而非元素。
指向同一数组的两个变量观察同一份元素写入。

- **赋值**：`b = a` 使 `b` 指向同一数组对象。
- **参数传递**：传递引用；被调方的元素写入对调用方可见。
- **返回值**：返回引用。
- **作为字段/元素**：以引用存储（见 [结构体](struct.md)、[类](class.md)、
  [内建泛型类](builtin-generic-classes.md) 的 `List<int[]>`）。

struct 类型的数组在行为上是例外：每个元素是值类型，所以 `new Point[n]` 为
每个元素物化一份独立的 `Point`，读取元素时深拷贝。见 [结构体](struct.md)
「值语义」。完整的值 vs 引用摘要见 [类型语义](type-semantics.md)。

### 长度

`a.length` 是**字段**（不是方法）——与 `string.length()`（方法，返回字节数）
对比。见 [字符串](string.md)。

### 下标读写

`arr[i]` 读写元素，下标是表达式（可嵌套）：

```nlang
int[] a = new int[3];
a[0] = 9;
a[1] = a[0] + 1;
int x = a[a.length - 1];
```

复合下标赋值（`arr[i] += 1`）不支持——请写 `arr[i] = arr[i] + 1`。见
[复合赋值](compound-assignment.md)。

### 越界

索引越界时读或写都抛 `IndexOutOfBoundsException`（运行期错误，可被
`try/catch` 捕获；未捕获时程序以退出码 1 终止——内建异常表见
[异常](exception.md)）。

### 数组值可去向

数组值在标量上下文中的去向规则见 [已知限制](known-limitations.md)
「标量上下文中的数组值」。迭代数组元素用 `foreach`（见
[foreach](foreach.md)）。用字面量初始化见 [集合初始化器](collection-initializers.md)。
