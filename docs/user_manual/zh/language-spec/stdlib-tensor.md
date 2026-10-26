# tensor——数值数组内核

`tensor`包提供四种标量元素类型(`int`/`long`/`float`/`double`)上的数值内核。内核以native代码运行于`nlang_tensor.dll`——数组载荷经钉住视图直接读写,一次内核调用替代整个解释循环(阶段1实测:逐元素加法约为手写循环的49倍,dot约为117倍)。

**命名一致**:所有族都用同名重载——编译器按实参类型选定声明,元素类型来自带类型的实参(`full(n, value)`取自value,`arange(start, stop, step)`取自等差三元组,数组族取自数组操作数)。精确实参匹配胜过加宽:`arange(0, 10, -3)`得到`int[]`;要`long[]`请传long实参。零填充数组无需内核——语言的尺寸创建`T[n]`本就零初始化(如`double[256]`),"全1"即`full(n, 1)`。下两表即本包全部公开面;`stdlib/tensor.n`中带后缀的声明(`fullI`、`arangeD`等)是重载的内部实现,不属于公开面。

| 函数 | 签名 | 说明 |
|------|------|------|
| full | (int n, T value) → T[] | n份value |
| arange | (T start, T stop, T step) → T[] | 半开等差序列,见下 |
| add sub mul div | (T[], T[]) → T[] | 逐元素二元运算,同名重载 |
| sum | (T[]) → T | 空数组返回0 |
| max min | (T[]) → T | 空数组抛Exception |
| mean | (T[]) → double | 恒为double,空数组抛Exception |
| dot | (T[], T[]) → T | int/long重载返回long(64位累加) |

**arange**遵循半开规则:元素为`start`、`start+step`……严格小于`stop`——`arange(0.0, 1.0, 0.25)`得到`0.0, 0.25, 0.5, 0.75`。步长为零、或无法向`stop`推进时,抛基类Exception。

**错误语义**与语言内建一致:null数组抛`NullPointerException`,创建长度为负或超过1亿元素上限抛`IndexOutOfBoundsException`,整型`div`除零抛`DivByZeroException`(float/double除法遵循IEEE,得Inf/NaN),长度不匹配抛基类Exception,空数组规则见上表。它们都是可捕获的`Exception`家族值。

## 矩阵与二维内核

二维面在`T[,]`操作数上沿用一维同名内核,矩阵族再补形状变换与乘积:

| 函数 | 签名 | 说明 |
|------|------|------|
| add sub mul div | (T[,], T[,]) → T[,] | 逐元素二元运算,形状须相等 |
| sum | (T[,]) → T | 按行主序摊平 |
| max min | (T[,]) → T | 摊平;空矩阵抛Exception |
| mean | (T[,]) → double | 恒为double,空矩阵抛Exception |
| transpose | (T[,]) → T[,] | [r, c]变[c, r] |
| flatten | (T[,]) → T[] | 行主序拷贝到一维 |
| reshape1..8 | (T[], int[] shape) → T[...] | 目标维度由名字承载,见下 |
| matmul | (T[,], T[,]) → T[,] | 矩阵乘;int/long回绕,float/double遵循IEEE |

**transpose**与**flatten**都是块拷贝——`transpose`把`[r, c]`映到`[c, r]`,`flatten`按行主序拷成一维数组。

**reshape1**–**reshape8**接受扁平的`T[]`源(更高维矩阵先用`flatten`适配)和`int[]`形状:形状长度须等于名字承载的目标维度数,乘积须等于元素总数;维度为负、长度不符或乘积不符抛基类Exception,并报出两侧数值。

**matmul**要求两个二维操作数且内维一致(`a`的列数=`b`的行数);不符抛基类Exception并报出双方形状(`tensor.matmul: shape mismatch (2x3 vs 2x2)`)。二维逐元素内核要求形状相等,报法相同(`tensor: shape mismatch (2x3 vs 2x2)`)。

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,] t = tensor.transpose(m);      // [[1, 3], [2, 4]]
int[] f = tensor.flatten(m);         // [1, 2, 3, 4]
int[,] p = tensor.matmul(m, m);      // [[7, 10], [15, 22]]
```

```nlang
import io;
import tensor;

int main() {
    double[] a = tensor.arange(0.0, 1.0, 0.25);    // 0.0 0.25 0.5 0.75
    double[] b = tensor.full(4, 2.0);
    double[] c = tensor.add(a, b);                 // 2.0 2.25 2.5 2.75
    io.print(tensor.sum(c));                       // 9.5
    io.print(tensor.dot(a, a));                    // 0.625
    return 0;
}
```
