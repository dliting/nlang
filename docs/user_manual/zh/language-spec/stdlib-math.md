# math——25个函数

浮点函数族全部以**double**精度运行：实参与返回值都是`double`，整型与`float`实参经隐式加宽进入（`math.sqrt(4)`、`math.sin(1.5f)`均可编译）。`sin`/`cos`/`tan`接受弧度；`asin`/`acos`/`atan`返回弧度。`log`是自然对数。`floor`/`ceil`/`round`返回**long**（`round`为四舍五入远离零）。

| 函数 | 签名 | 说明 |
|----------|-----------|-------|
| sin cos tan asin acos atan | (double) → double | 弧度 |
| atan2 | (double y, double x) → double | C/C++实参顺序 |
| sqrt pow exp log | (double[,double]) → double | pow(x,y)；log = ln |
| absi / absf | (int)→int / (double)→double | absi(int最小值)抛错 |
| mini maxi / minf maxf | (T, T) → T | int对 / double对 |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (double) → long | 超int64或NaN → Exception |
| random | () → double | [0,1)，伪随机数生成器（PRNG，pseudorandom number generator）见下 |
| srand | (int) → void | 重新播种 |
| randomi | (int min, int max) → int | 闭区间；min > max → Exception |

**PRNG确定性**：`std::mt19937`，程序启动时由`std::random_device`播种。`math.srand(n)`显式重播种——此后序列完全确定，且跨平台一致：

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24 位尾数，[0,1) 内精确
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // 存在小模偏差，已记录在案
```

不使用`std::uniform_*_distribution`——它们是实现定义的，不可移植。
