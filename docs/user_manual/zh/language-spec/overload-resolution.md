# 重载解析


当存在多个重载时，编译器通过计算类型距离得分来选出最佳匹配。如果有两个或更多重载以相同距离匹配，该调用是二义的，会报编译错误。

```nlang
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK：第二个重载（2 个实参匹配 2 个形参）
foo(5);       // 错误：二义（两个重载都接受 1 个实参）
```

### 构造器重载

class与struct重载构造器的方式和重载方法一致：候选集是该类型的全部构造器——class贡献其以类名命名的非静态成员，struct贡献其声明的构造器——调用绑定到绑定距离最小的候选（距离相同即二义）。实参按位置匹配，因此构造器调用不接受命名实参或`out`实参。

```nlang
class Shape {
    public int tag;
    public Shape() { this.tag = 0; }
    public Shape(int r) { this.tag = 10 + r; }
    public Shape(string name) { this.tag = 2; }
}
Shape a = Shape();    // 绑定 Shape()
Shape b(3);           // 绑定 Shape(int)
Shape c = Shape("n"); // 绑定 Shape(string)——字符串字面量不转换为int，
                      // 只有string构造器能匹配
```

默认参数参与距离计算：精确类型匹配（距离0）优先于加宽一跳，收窄实参会拒绝对应候选。

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

导入的自由函数、接口静态方法、枚举方法的重载，解析方式与同单元内完全一致：编译器在调用点按参数类型选出最佳匹配，链接器把选中的那个重载连到生产方单元里它自己的记录。因此同名同参数数但参数类型不同的两个重载不会再在链接期产生歧义——调用点已经决定了运行哪一个。

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
