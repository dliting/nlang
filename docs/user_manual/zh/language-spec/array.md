# 数组


数组是定长、可按索引寻址的序列。语法为`T[]`。

### 创建数组

```nlang
int[] a = int[3];         // 表达式位：定长创建，元素零初始化
int[] b = [1, 2, 3];      // 裸方括号字面量
int[] e = [];             // 空数组字面量（int[0]等价）

int d[3];                 // 声明位定长：零填充
int f[100] = [7, 8];      // 声明位部分初始化：余位零填充
```

- **表达式位**：`int[3]`在堆上创建定长数组，元素零初始化；`[1, 2, 3]`是集合字面量形式（花括号形式`{...}`保留给集合、struct与class初始化——`int[] c = {1, 2};`被拒绝），推断与消歧规则见[集合初始化器](collection-initializers.md)。
- **声明位**：`int d[3];`直接声明定长数组并零填充；带初始化器时未列出的元素零填充（`int f[100] = [7, 8];`只填前两个）。尺寸只允许出现在**首个声明符**上——`int a, b[3];`是语法错误；混合声明符的类型继承规则见[已知限制](known-limitations.md)。
- 只声明不初始化（`int[] g;`）得到null引用，见[声明](declarations.md)的默认值表。
- 元素类型可以是struct（`Point[3]`为每个元素创建独立值，见[结构体](struct.md)）或class（存引用）。数组的数组（锯齿形态）不支持，见[已知限制](known-limitations.md)。

### 引用语义

数组是**引用类型**：数组在堆上，赋值与参数传递拷贝数组**引用**而非元素。指向同一数组的两个变量观察同一份元素写入。

- **赋值**：`b = a`使`b`指向同一数组对象。
- **参数传递**：传递引用；被调方的元素写入对调用方可见。
- **返回值**：返回引用。
- **作为字段/元素**：以引用存储（见[结构体](struct.md)、[类](class.md)、[内建泛型类](builtin-generic-classes.md)的`List<int[]>`）。

struct类型的数组在行为上是例外：每个元素是值类型，所以`Point[n]`在创建时为每个元素分配一份独立的`Point`，读取元素时深拷贝。见[结构体](struct.md)「值语义」。完整的值vs引用摘要见[类型语义](type-semantics.md)。

### 长度

`a.length()`是方法（无参数、返回`int`），与`string.length()`、`List.length()`形态一致。见[字符串](string.md)。

### 下标读写

`arr[i]`读写元素，下标是表达式（可嵌套）：

```nlang
int[] a = int[3];
a[0] = 9;
a[1] = a[0] + 1;
int x = a[a.length() - 1];
```

下标左值的复合赋值（`arr[i] += 1`）受[复合赋值](compound-assignment.md)同一规则约束（元素为string时仅`+=`）。

### 越界

索引越界时读或写都抛`IndexOutOfBoundsException`（运行期错误，可被`try/catch`捕获；未捕获时程序以退出码1终止——内建异常表见[异常](exception.md)）。

### 数组值可去向

数组值在标量上下文中的去向规则见[已知限制](known-limitations.md)「标量上下文中的数组值」。迭代数组元素用`foreach`（见[foreach](foreach.md)）。用字面量初始化见[集合初始化器](collection-initializers.md)。
