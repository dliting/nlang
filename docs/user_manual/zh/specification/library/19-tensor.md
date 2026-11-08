> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 19. tensor包

`tensor`包针对`int`、`long`、`float`与`double`四种元素种类的数组提供数值核函数。张量值就是普通数组（§4.10）：该包不引入新类型，元素用§5的下标读写，§4.10的数组成员原样适用。§19.1固定包形态；§19.2定义构造与形状操作；§19.3定义逐元素核函数与归约；§19.4定义矩阵乘积；§19.5定义示范层；§19.6固定错误模型。

## 19.1 包形态

`tensor`是标准库包（§3.9.4）：`import tensor;`把它带入作用域，每个函数都经包限定符调用（`tensor.add(a, b)`）。包名是单段的（§3.3）。它的声明是标准库的`.n`源码；核函数是由随工具链发布的原生库实现的`native`函数（§7.5）。每个核函数都是阻塞的原生函数：它声明阻塞段（§10.1），所以在核函数运行期间调用上下文停泊、载体锁被释放。实参准入是§14.1的包规则。

每个家族都使用同名重载：编译器按实参类型挑选声明，所以元素种类来自带类型的实参——`full`的填充值、`arange`的数列三元组、其余各处的数组操作数。精确的实参匹配优先于更宽的匹配：`arange(0, 10, -3)`选中`int`重载，传`long`实参则选中`long`重载。带后缀的按种类声明（`fullI`、`arangeD`及其家族）是重载的内部实现，不属于文档面。

## 19.2 构造与形状

- `full(n, value)`返回由`value`的`n`个副本构成的秩1数组；`full(dims, value)`改为填充由`dims`描述的形状，`dims`是秩1的`int[]`。元素类型是`value`的类型；家族的每个成员都按该类型挑选。
- `arange(start, stop, step)`返回严格在`stop`之前的半开等差数列`start`、`start + step`、……；元素类型是三个参数的公共类型。`arange(0.0, 1.0, 0.25)`得到`0.0, 0.25, 0.5, 0.75`。
- 零填充的数组不需要核函数：定长创建`T[n]`即零初始化（§4.10）；全1数组即`full(n, 1)`。
- `reshape(a, shape)`把`a`的元素按行主序读出并填入`shape`所述形状，`shape`是秩1的`int[]`；各维之积必须等于`a`的元素数，结果秩遵循§4.10的全局上限（带方括号标注的结果按标注核秩）。至多一个维度可为`-1`——由元素数除以其余各维之积推断，且必须整除。当元素本就按行主序无间隙地排列——程序创建的数组，或恰好连续选取元素的视图——结果是共享这些元素的视图；否则新建一份副本，结果绝不与`a`别名。
- `concat(a, b, axis)`沿`axis`拼接两个同秩数组：结果在`axis`维上取长度之和，其余各维取操作数的长度——这些长度必须相等。
- `flatten(a)`把任意秩数组的元素按行主序作为秩1数组返回——元素连续时是视图，否则是副本（与单维`reshape`同一条规则）。*资料性：*这对应NumPy的`ravel`而非NumPy的`flatten`——后者总是复制；NLang保留一词拼写与尽可能返回视图的语义。
- `transpose(a)`反转任意秩数组的各轴，`transpose(a, axes)`重排它们：`axes`是秩1的`int[]`，按结果读取的顺序把每个源轴各命名一次。两者都返回与`a`别名的视图——经转置元素的写入直达源（§4.10.9）。
- `slice(a, starts, stops, steps)`是切片下标（§4.10.9）的函数形式：三个秩1的`int[]`数组，每轴一个条目，归一化方式与视图结果都相同。
- `broadcastTo(a, shape)`返回`a`在`shape`所述形状下的视图，`shape`是秩1的`int[]`：目标各维必须等于源各维、或源该维为1（长度1的轴重复），并可在前面补足维度。重复的轴始终寻址同一个源单元；经跨多于一个位置的重复轴存储抛出`Exception`（§4.10.9），尺寸1的重复轴保持可写。元素读取、归约与序列化看到的是重复后的内容。

```nlang
import io;
import tensor;

int main() {
    int[] shape = int[2];
    shape[0] = 2; shape[1] = 3;
    double[,] m = tensor.full(shape, 2.5);   // 2x3，每个元素都是2.5
    int[] six = int[1];
    six[0] = 6;
    double[] flat = tensor.reshape(m, six);  // 单个维度 -> 秩1
    io.print(flat.length());                 // 6
    int[] inferred = int[2];
    inferred[0] = -1; inferred[1] = 2;
    double[,] rows = tensor.reshape(m, inferred);  // [-1, 2] -> [3, 2]
    io.print(rows.length());                 // 6
    return 0;
}
```

## 19.3 逐元素核函数与归约

- `add(a, b)`、`sub(a, b)`、`mul(a, b)`与`div(a, b)`逐元素施加运算符，返回操作数元素类型的数组。两个数组按§19.3.1广播；标量第二实参（`add(a, 0.5)`）施加到每个元素。整数`div`除零抛出`DivByZeroException`（§8.3）；`float`与`double`除法遵循IEEE，产出Inf或NaN（§4.2）。
- `exp(a)`与`log(a)`逐元素施加数学函数（`e`的元素次幂、自然对数），返回操作数元素类型的数组。它们只为`float`与`double`声明。
- `max(a, bound)`与`min(a, bound)`返回`a`的副本，每个元素在`bound`处被截断（从下方抬到界，从上方压到界）；元素类型是元素与界的公共类型。
- `sum(a)`以元素类型返回所有元素之和；空数组产出该类型的零值。`sum(a, axis)`沿一个轴求和并把它从形状中除去：秩2的`(3, 4)`沿轴0求和得到`(4,)`，秩1数组沿最后一个轴求和得到标量。
- `max(a)`与`min(a)`以元素类型返回最大与最小元素；空数组抛出`Exception`（§8.3）。
- `mean(a)`对每种元素类型都以`double`返回算术平均值；空数组抛出`Exception`。
- `dot(a, b)`返回两个等长数组的内积。`int`与`long`重载以64位累加并返回`long`；`float`与`double`重载返回自身类型。空数组产出零。

对秩2操作数，逐元素核函数保持操作数的形状，归约则按行主序把各元素摊平后进行。视图是普通操作数：每个核函数按视图自己的形状与步长读取（`transpose`的结果可直接喂给`matmul`），需要连续单元的核函数——`matmul`与`concat`——会先把非连续的操作数复制一份，程序察觉不到。

```nlang
import io;
import tensor;

int main() {
    double[] a = tensor.arange(0.0, 1.0, 0.25);    // 0.0 0.25 0.5 0.75
    double[] b = tensor.full(4, 2.0);
    double[] c = tensor.add(a, b);                 // 2.0 2.25 2.5 2.75
    io.print(tensor.sum(c));                       // 9.5
    io.print(tensor.dot(a, a));                    // 0.875
    double[] e = tensor.exp(c);                    // 逐元素e^x
    double[] lo = tensor.max(a, 0.5);              // 0.5 0.5 0.5 0.75
    return 0;
}
```

### 19.3.1 广播

双数组核函数按右对齐的广播规则配对元素，与NumPy的规则相同：秩可以不同——维数较少的操作数在前面补上长度1的轴——每个对齐轴上两个长度必须相等、或其中之一为1；结果在每个轴上取较大的长度，长度1的操作数轴重复其单一取值。

```nlang
double[,] m = tensor.full([3, 4], 0.0);   // dims的写法见§19.2
double[] bias = tensor.full(4, 1.0);
double[,] biased = tensor.add(m, bias);   // (3,4)+(4,)：各行重复
```

既不相等也不为1的轴对抛出`Exception`，消息携带两个形状（§19.6）。

## 19.4 矩阵核函数

`matmul(a, b)`返回两个秩2数组的矩阵乘积：`[aRows, aCols] * [bRows, bCols]`产出`[aRows, bCols]`，且内部维度必须匹配。算术遵循全语言统一的规则（§4.2）：`int`与`long`回绕，`float`与`double`遵循IEEE。

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,] t = tensor.transpose(m);      // [[1, 3], [2, 4]]
int[] f = tensor.flatten(m);         // [1, 2, 3, 4]
int[,] p = tensor.matmul(m, m);      // [[7, 10], [15, 22]]
```

## 19.5 示范层

该包的三个函数用NLang本身编写——其源码是该包的`.n`文件，像任何库函数一样可读可改（§3.9.4）。它们组合native核函数，而不是新增核函数：

- `relu(a)`从下方把数组截到零（`max(a, 0)`），为`float`与`double`声明——整数元素没有激活语义。
- `softmax(x)`对平移后的元素（`x - max(x)`，防止大输入让`exp`溢出）取指数，再按它们的和归一；结果总和为1。
- `crossEntropyLoss(logits, labels)`计算独热`labels`对`softmax(logits)`的交叉熵：`-sum(labels * log(softmax(logits)))`。形参是秩1的：秩2的批次先经`flatten`摊平（§19.2）。

```nlang
import io;
import tensor;

int main() {
    double[] logits = [2.0, 1.0, 0.1];
    double[] labels = [1.0, 0.0, 0.0];
    double loss = tensor.crossEntropyLoss(logits, labels);
    io.print(loss);                    // ≈ 0.417（argmax的软目标）
    double[] shifted = tensor.sub(logits, [1.0, 1.0, 1.0]);  // [1, 0, -0.9]
    double[] act = tensor.relu(shifted);                     // [1, 0, 0]
    io.print(act[2]);                  // 0——截到零
    return 0;
}
```

## 19.6 错误行为

每次张量失败都是`Exception`家族的一个可捕获值（§8.3）。`null`数组实参抛出`NullPointerException`。负的创建长度、或越过§4.10创建上限的元素数，在创建位点抛出`IndexOutOfBoundsException`；负的`reshape`维度（单个推断的`-1`除外）抛出同一个类。`arange`在生成之前检查其元素数，数列会超出上限时抛出`Exception`。零的`arange`步长、或无法朝`stop`推进的步长，抛出`Exception`。双操作数核函数中的广播不匹配、`reshape`的形状其长度与目标秩不符、其乘积与元素数不符或其`-1`推断不整除、`transpose`的重排漏掉或重复某个轴、`broadcastTo`的目标形状源形状无法填满、`sum`的轴在数组的秩之外、`slice`的列表长度与秩不符、`concat`的操作数在秩或非`axis`维上不一致，以及对空数组调用`max`、`min`或`mean`，都抛出`Exception`；形状与长度不匹配的消息会点名两侧，轴类消息会点名轴与秩。

*资料性：*诊断消息的写法形如`matmul`的`tensor.matmul: shape mismatch (2x3 vs 2x2)`、逐元素核函数的`tensor: shape mismatch (2x3 vs 2x2)`。

> 实现注记。核函数在`nlang_tensor.dll`中以原生代码运行，经由固定视图读写数组载荷，所以一次核函数调用取代一整个解释器循环。*资料性：*对随包发布的构建实测，逐元素加法约为手写循环的49倍，`dot`约为117倍；这些数字描述当前实现，并非语言承诺。
