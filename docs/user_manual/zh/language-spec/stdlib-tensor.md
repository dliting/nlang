# tensor——数值数组内核

`tensor`包提供四种标量元素类型(`int`/`long`/`float`/`double`)上的数值内核。内核以native代码运行于`nlang_tensor.dll`——数组载荷经钉住视图直接读写,一次内核调用替代整个解释循环(阶段1实测:逐元素加法约为手写循环的49倍,dot约为117倍)。

**创建函数带元素类型后缀**(`I`/`L`/`F`/`D`):其实参无法透露期望的元素类型。**逐元素与归约族使用同名重载**——编译器按实参类型选定声明,native侧按运行时元素类型分派。

| 函数 | 签名 | 说明 |
|------|------|------|
| zerosI zerosL zerosF zerosD | (int n) → T[] | 零初始化 |
| onesI onesL onesF onesD | (int n) → T[] | 每个元素为1 |
| fullI fullL fullF fullD | (int n, T value) → T[] | n份value |
| arangeI arangeL arangeF arangeD | (T start, T stop, T step) → T[] | 半开等差序列,见下 |
| add sub mul div | (T[], T[]) → T[] | 逐元素二元运算,同名重载 |
| sum | (T[]) → T | 空数组返回0 |
| max min | (T[]) → T | 空数组抛Exception |
| mean | (T[]) → double | 恒为double,空数组抛Exception |
| dot | (T[], T[]) → T | int/long重载返回long(64位累加) |

**arange**遵循半开规则:元素为`start`、`start+step`……严格小于`stop`——`arangeD(0.0, 1.0, 0.25)`得到`0.0, 0.25, 0.5, 0.75`。步长为零、或无法向`stop`推进时,抛基类Exception。

**错误语义**与语言内建一致:null数组抛`NullPointerException`,创建长度为负抛`IndexOutOfBoundsException`,整型`div`除零抛`DivByZeroException`(float/double除法遵循IEEE,得Inf/NaN),长度不匹配抛基类Exception,空数组规则见上表。它们都是可捕获的`Exception`家族值。

```nim
import io;
import tensor;

int main() {
    double[] a = tensor.arangeD(0.0, 1.0, 0.25);   // 0.0 0.25 0.5 0.75
    double[] b = tensor.fullD(4, 2.0);
    double[] c = tensor.add(a, b);                 // 2.0 2.25 2.5 2.75
    io.print(tensor.sum(c));                       // 9.5
    io.print(tensor.dot(a, a));                    // 0.625
    return 0;
}
```
