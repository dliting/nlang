> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 7.3 重载解析

同一裸池中的顶层函数可以重载：同名、不同参数表（§3.7）。类与结构体成员以其方法重载的方式同样重载构造器（§4.7，§4.8）。

在调用点，编译器在参数数能与实参表匹配的候选者中选择，由默认值填充未绑定的形参（§7.2）。当每个实参都能经隐式转换可赋值给其参数时，候选者是可行的（§4.4）；需要收窄转换的实参使候选者不可行。每个可行候选者按其各实参的转换距离之和计分：完全匹配贡献零，每个隐式转换步贡献一跳。总分最小的候选者被选中。两个或多个候选者在最小总分上打平是歧义，没有可行候选者的调用是错误：两者都是编译期错误。

默认参数不打破打平：把某个形参留给默认值的候选者，与恰好绑定该形参的候选者得分相同，所以`f(1)`面对`f(int)`与`f(int, int = 5)`是歧义的。

构造器调用按同样的距离规则在该类型的各构造器之间解析；其实参按位置匹配，因此不接受命名与`out`实参（§7.2）。

导入函数的各重载在调用点对照导入的签名解析，与单元内部完全一致；链接器把选中的重载连到其在产出单元中的记录（§3.9）。

### 重载选择

*资料性：*距离得分在通过可行性检验的候选者之间做出裁决；下面的例子会报告二义。

```nlang
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK：第二个重载（2个实参匹配2个形参）
foo(5);       // 错误：二义（两个重载都接受1个实参）
```

### 构造器重载

*资料性：*候选集是该类型的全部构造器——class贡献其以类名命名的非静态成员，struct贡献其声明的构造器——调用绑定到绑定距离最小的候选（距离相同即二义）。

```nlang
class Shape {
    public int tag;
    public Shape() { this.tag = 0; }
    public Shape(int r) { this.tag = 10 + r; }
    public Shape(string name) { this.tag = 2; }
}
Shape a = Shape();    // 绑定Shape()
Shape b(3);           // 绑定Shape(int)
Shape c = Shape("n"); // 绑定Shape(string)——字符串字面量不转换为int，
                      // 只有string构造器能匹配
```

默认参数参与距离计算：精确类型匹配（距离0）优先于一级加宽，收窄实参会拒绝对应候选。

```nlang
class V {
    public V(int a, int b = 7) { }
    public V(float a) { }
}
V a = V(1);     // V(int, int)距离0，优先于V(float)的int→float加宽
V b = V(1.5f);  // V(float)——float实参不收窄到int
```

尾随默认值不能打破平局：`W(1)`以距离0同时绑定`W(int)`与`W(int, int b = 5)`，是真正的二义——

```nlang
class W {
    public W(int a) { }
    public W(int a, int b = 5) { }
}
W w = W(1);   // 错误：ambiguous constructor call to class "W":
              //           multiple constructors match the argument list
```

——当没有任何候选接受该实参列表时：

```nlang
class U {
    public U(int a) { }
}
U u = U("s");   // 错误：no constructor of class "U" matches the argument list
```

struct的诊断文案中用`struct`（`no constructor of struct "S" matches the argument list`、`ambiguous constructor call to struct "S": ...`）。

### 跨模块重载

*资料性：*导入的自由函数、接口静态方法、枚举方法的重载，解析方式与同单元内完全一致：编译器在调用点按参数类型选出最佳匹配，链接器把选中的那个重载连到生产方单元里它自己的记录。因此同名同参数数但参数类型不同的两个重载不会再在链接期产生歧义——调用点已经决定了运行哪一个。

```nlang lib.n
int score(int v) { return v + 1; }
int score(double v) { return 100; }
```

```nlang
import lib;

int a = lib.score(1);     // int重载    → 2
int b = lib.score(2.5);   // double重载 → 100
```

编译期二义规则不变：当实参以相同距离可转换到两个重载的形参类型时，解析器在调用点报告二义，无论这两个重载是否位于另一个模块。
