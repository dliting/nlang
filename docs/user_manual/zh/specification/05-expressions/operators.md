> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 5.10 运算符

规范性规则见§5.1–§5.3。

### 算术

```nlang
a + b    a - b    a * b    a / b    a % b
```

整数除法向零截断。除以零（包括取模）抛运行期错误——这包括浮点除零，它抛出而不是产生IEEE 754的±inf/NaN（NLang在此处与C/C++/Java不同）。`%`对浮点操作数取fmod余数（`7.5 % 2.0` = 1.5）。

**整数溢出**以二进制补码静默回绕（C风格）：`INT_MAX + 1 == INT_MIN`。没有SafeInt式的检查。

**数值提升**：窄于int的整型操作数先提升为int；随后结果类型是能同时隐式接收两个操作数的最小类型：

- `int + int` → `int`；`byte + byte` → `int`
- `int + uint` → `long`；`long + uint` → `long`
- `int`/`long`与`ulong`混合 → 编译错误（先显式统一符号域）
- `int + float` / `float + int` → `float`（对称）；任一操作数为`double`
  → `double`

所以`1 + 2.5 == 2.5 + 1 == 3.5`（对称）。结果类型是提升后的类型；赋回更窄的类型需要显式`as`（常量适配特例除外）。bool与char不参与算术。完整的提升与转换规则见[类型](../04-types/index.md)。

`string + string`是拼接；`"x" + 'y'`（char隐式转string）与`+`标量操作数同样拼接。`string - string`等是编译错误。见[字符串](../04-types/string.md)。

泛型上下文内的类型参数值同样拼接：`"v=" + item`与`item + "!"`两序都按实参的种类转换后并入（§4.12.9）。

### 数组算术

`+`、`-`、`*`、`/`的操作数之一或两者为数值数组（int、long、float、double元素）时，运算符下沉到[tensor](../library/18-tensor.md)包的内核——`a + b`即`tensor.add(a, b)`，按右对齐广播；`2 * x`、`x - 1`、`1 - x`走标量臂（`1 - x`下沉到反向臂`tensor.rsubScalar`，因为`-`无交换律）；`-a`即`tensor.neg`。两个数组操作数必须携带相同元素种类——数组间不做静默加宽；标量一侧按普通规则转换。运算符是`tensor.*`调用的拼写：包必须先导入，缺失导入按缺失诊断。数组上的复合赋值（`a += b`）按编译错误拒绝——请使用运算符形式的普通赋值。`string + ...`保持拼接语义，数组恒等比较（`==`/`!=`）不变。见[张量](../library/18-tensor.md)§18.5.1。

### 位移

```nlang
a << b    a >> b
```

按位左移与右移。**左操作数决定结果类型**（Java/C#规则）：窄于int的整型左操作数先单目提升为int（`byte << 2`是`int`）。右操作数（计数）按普通隐式转换规则进入结果类型（见[类型](../04-types/index.md)）：int计数适用于结果类型int与long，但不适用于uint或ulong；long计数只适用于long；能装进结果类型的常量计数对任何整型结果都合法。其余情形是编译错误，诊断会点名结果类型与计数两者的类型（`1 << long计数`）。enum成员按其int32底层类型参与（`Color.Green << 1 == 2`）。float、double、bool、char、string操作数是编译错误（`operator '<<' requires integer operands`）；null操作数报`null is not a valid shift operand`。

有符号类型`>>`是算术右移（符号扩展，`-8 >> 1 == -4`）；无符号类型是逻辑右移（零填充）。`<<`按二进制补码回绕（`1 << 31`在int上是`INT_MIN`）。计数必须在`[0, 位数)`内——负数或达到类型位宽的计数抛运行期错误（与除零同类），不做C的未定义行为，也不做Java的掩码回绕。

优先级介于关系与加减之间：加法先于位移（`1 << 2 + 3`是`1 << 5`），位移先于关系（`1 << 2 < 8`比较的是`4 < 8`）。

**复合形**`<<=`/`>>=`以左值的声明类型存回——不做单目提升（`byte b <<= 2`仍是byte，`1 <<= 7`在有符号byte上回绕为-128）——存回与`+=`族同为隐式窄化。计数遵循上文二元形的进入规则（按普通隐式转换进入结果类型，常量计数只要值装得下结果类型就放行，`[0, 位数)`运行期检查），所以`b <<= 200`是编译错误，负数变量计数抛具名运行期错误。支持的左值即复合赋值族（局部变量、字段、下标元素），见[复合赋值](../06-statements/index.md)。

泛型实参列表内的`>>`仍是嵌套类型的收尾（`List<List<int>>`）；只有出现在表达式位置时它才是位移运算符，两者不会冲突。

### 位运算符

```nlang
a & b    a ^ b    a | b    ~a
```

二元位运算符`&`（按位与）、`^`（按位异或）、`|`（按位或）要求整型操作数；enum成员字面量按其底层int32参与，enum类型变量被拒绝（与位移同一口径），char、bool与浮点操作数被编译器拒绝，需显式`as`转换参与。结果类型按算术族对称提升规则取共同类型：`int & long`是long，`byte & byte`是int。与位移不同，位运算符没有「计数」语义，两个操作数对等参与提升：`-1 & 1099511627776`合法、结果是long且bit40保留（整数字面量没有类型后缀，超出int范围的字面量自动取long承载）。一元`~`（按位取补）对操作数施以与二元同款单目提升：`byte`、`ubyte`、`short`、`ushort`提升为`int`，`int`、`uint`、`long`、`ulong`保持自身；`~5`是`-6`，遵循二进制补码。位运算没有运行期范围检查，也没有溢出。

优先级：`~`与其他单目运算符同级；`&`、`^`、`|`依次松于相等比较、严于逻辑运算`&&`与`||`——`5 & 3 == 1`按`5 & (3 == 1)`读，是C族惯例。复合形`&=`、`|=`、`^=`随复合赋值族整体规则：按左值声明类型存回、隐式窄化，与`+=`、`<<=`一致，见[复合赋值](../06-statements/index.md)。

### 比较

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

比较结果一律是**`bool`**（`true` / `false`）。

**比较操作数是有类型的**：

- **string/非string混合是编译错误**（`"a" < 5`、`5 == "a"`），null字面量也不例外——`s == null`是编译错误（null不是字符串值，§4.3）。`c == null`对class/引用操作数与null哨兵做同一性比较。
- **数值对**得到与算术相同的对称提升：`-2 < -1.5`提升为double且为真；`1 == 1.0`为真。见[基本类型](../04-types/primitives.md)。
- **char只与char比较**（`'a' < 'b'`合法）；char与数值比较不经隐式转换，先`c as int`取码点（`a char value can only be compared with a char value`）。
- **bool只与bool比较，且只比较相等**：`true == true`为真；对bool排序是编译错误（`bool values cannot be ordered; only == and != are supported`），bool与其他类型比较同样是编译错误（`a bool value can only be compared with a bool value`）。
- **字符串相等**按内容比较；字符串关系序用C `strcmp`式的逐字节比较（例如`"Z" < "a"`为真，因为`'Z'`（90）< `'a'`（97））。因为字符串以Unicode转换格式（UTF-8，Unicode Transformation Format）存储且UTF-8字节序等于码点序，对美国信息交换标准代码（ASCII，American Standard Code for Information Interchange）之外的文本排序也正确：`"é" > "z"`为真。见[字符串](../04-types/string.md)「比较」。
- **class/引用相等**（`==`、`!=`）是同一性（同一堆对象）。要按值比较，用`equals`方法并在自己的类里覆写它——`==`不受覆写影响；见[Object与装箱](../04-types/object.md)「协议方法」与[类](../04-types/class.md)「虚方法与重写」。
- **结构体相等**（`==`、`!=`）按字段值比较——同类型且字段全部相等的两个结构体相等（嵌套结构体字段递归，`string`字段比较内容，引用形态字段比较同一性）。对结构体排序是编译错误（`struct values cannot be ordered; only == and != are supported`），与另一类型的结构体、标量或null比较同样是编译错误——结构体永不为null（`a struct value can only be compared with a struct value of the same type`）。见[结构体](../04-types/struct.md)。
- **函数相等**（`==`、`!=`）比较句柄内容：指向同一函数的两个引用相等；排序是编译错误。见[一等函数值](../../vm-architecture/first-class-function-values.md)。
- **类型参数相等**（泛型类方法体内、`static`成员体内或泛型函数体内）：`==`、`!=`作用于`T`操作数——`T`与`T`、`T`与`null`两个方向——按实例化的实参种类分派：标量按值、`string`按内容、结构体按字段值、类与数组按恒等、枚举按内联值。关系序（`<`等）对类型参数是编译错误。见[泛型](../04-types/generics.md)「运行时类型信息与类型参数」。
- **带null操作数的算术/拼接**是编译错误——null只有通过上面的比较同一性路径才有值。

### 逻辑

```nlang
a && b   a || b   !a
```

短路，C风格：`&&`仅在`a`为`true`时求值`b`；`||`仅在`a`为`false`时求值`b`。被跳过的操作数完全没有可观察效果——没有调用、没有抛出。结果永远是**`bool`**（绝不是原始操作数值）。`&&`、`||`、`!`的操作数必须是`bool`（`operator '&&' requires bool operands`）——比较与谓词已经产生bool。见[语句](../06-statements/index.md)「条件类型」。
