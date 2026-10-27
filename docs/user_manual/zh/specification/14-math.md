> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 14. math包

## 14.1 包形态

`math`是标准库包（§12.4）：`import math;`把它带入作用域，每个函数都经包限定符调用（`math.sqrt(2)`）。包名是单段的（§3.3）。它的声明是标准库的`.n`源码；实现是`native`函数（§7.5），由以该包命名的宿主原生库承载。

## 14.2 精度

包中每个浮点函数都以`double`精度运行：参数与结果都是`double`（§4.2）。整数与`float`实参经§4.12的隐式加宽进入，所以`math.sqrt(4)`与`math.sin(1.5f)`都能编译。`double`结果只有经显式的`as`转换才能到达`float`目标（§5.5）。

## 14.3 函数

该包声明25个函数。

| 函数 | 签名 | 效果 |
| --- | --- | --- |
| `sqrt(x)` | (`double x`) → `double` | `x`的非负平方根 |
| `sin(x)` / `cos(x)` / `tan(x)` | (`double x`) → `double` | 弧度`x`的三角比 |
| `asin(x)` / `acos(x)` | (`double x`) → `double` | 正弦（`asin`）或余弦（`acos`）为`x`的弧度 |
| `atan(x)` | (`double x`) → `double` | 正切为`x`的弧度 |
| `atan2(y, x)` | (`double y`, `double x`) → `double` | 从原点到点`(x, y)`方向的弧度 |
| `pow(x, y)` | (`double x`, `double y`) → `double` | `x`的`y`次幂 |
| `exp(x)` | (`double x`) → `double` | `e`的`x`次幂 |
| `log(x)` | (`double x`) → `double` | `x`的自然对数 |
| `absi(x)` | (`int x`) → `int` | `x`的绝对值 |
| `absf(x)` | (`double x`) → `double` | `x`的绝对值 |
| `mini(a, b)` / `maxi(a, b)` | (`int a`, `int b`) → `int` | 较小（`mini`）或较大（`maxi`）的实参 |
| `minf(a, b)` / `maxf(a, b)` | (`double a`, `double b`) → `double` | 较小（`minf`）或较大（`maxf`）的实参 |
| `clampi(v, lo, hi)` | (`int v`, `int lo`, `int hi`) → `int` | 把`v`强制进闭区间`[lo, hi]` |
| `clampf(v, lo, hi)` | (`double v`, `double lo`, `double hi`) → `double` | 把`v`强制进闭区间`[lo, hi]` |
| `floor(x)` | (`double x`) → `long` | `x`向负无穷舍入 |
| `ceil(x)` | (`double x`) → `long` | `x`向正无穷舍入 |
| `round(x)` | (`double x`) → `long` | `x`按半数远离零舍入 |
| `random()` | () → `double` | 生成器的下一个值，落在`[0, 1)`（§14.5） |
| `srand(seed)` | (`int seed`) → `void` | 为生成器重新播种（§14.5） |
| `randomi(min, max)` | (`int min`, `int max`) → `int` | 从闭区间`[min, max]`抽取的值（§14.5） |

## 14.4 错误行为

失败分为两种模型。

- 超越函数家族的定义域错误——对负数取`sqrt`、对非正数取`log`、`asin`或`acos`的实参落在`[-1, 1]`之外——产出一个NaN的`double`（§4.2），不抛出；该包遵循C惯例。NaN会经由后续算术传播下去。
- 实参错误抛出`Exception`（§9.3）：对最负的`int`调用`absi`；`lo > hi`时的`clampi`或`clampf`；`min > max`时的`randomi`；以及`x`为NaN或舍入结果落在`long`值域之外时的`floor`、`ceil`或`round`。

## 14.5 随机数

该包在每次程序运行中从同一个生成器抽取。

- `random()`产出2^24个不同值之一，精确缩放到`[0, 1)`。
- `randomi(min, max)`按取模归约从闭区间抽取；大小不能整除2^32的区间带有轻微偏置。
- `srand(seed)`之后，序列由种子固定，且跨平台一致。任何`srand`调用之前生成器的状态是*未指明*的（§0.4）。

> 实现注记。随包发布的生成器是梅森旋转（Mersenne Twister）`std::mt19937`，在程序启动时由`std::random_device`播种；`random()`取一次抽取的高24位，`randomi`对一次抽取按区间大小取模。
