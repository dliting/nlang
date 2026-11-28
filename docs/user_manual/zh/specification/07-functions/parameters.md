> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 7.2 参数与实参

形式参数属于函数作用域（§3.5），在每次调用时由对应的实参初始化。初始化遵循第4章的值模型：结构体形参收到副本（§4.7），而类、接口、数组或函数形参与实参指代同一个对象（§4.10、§4.8、§7.4）。实参必须可赋值给参数类型（§4.4）。参数与局部变量一样可赋值（§6.2）。

形参可以在任意位置声明默认值`T p = e`（§2.4）。默认表达式在声明的作用域中解析，在调用点求值，于每次省略该实参的调用时求值；它可以引用同一声明中更早的形参。该值必须可赋值给`T`（§4.4）。默认值属于声明：经模块导入的调用以同样的方式填充它们，包括引用更早形参或声明单元中函数的默认值。

调用可以按形参名字绑定实参：`f(name = expr)`。按位置的实参排在所有命名实参之前；命名实参可以按任意顺序出现，按名字而非位置绑定。每个形参至多绑定一次。不是所选候选者（§7.3）的形参的名字是编译期错误。命名实参从左到右求值（§5.1）。构造器调用只接受按位置的实参（§7.3）。

### 默认值

*资料性：*默认值可以出现在任意位置（不限于末尾）。默认表达式可以引用更前面的形参。

```nlang
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // 引用更前面的形参
int foo(int a, int b = 0, int c) { return c; }     // 默认值不在末尾
int apply(func<int, int> g = (x) -> x + 1) { return g(1); }   // 函数值默认值
```

- `foo(5)` → `b`取默认值
- `foo(5, 10)` → `b`为10，默认值不评估
- 默认表达式在调用点求值（不是在声明处）
- 默认表达式与形参类型不匹配是编译错误
- 形参类型是函数类型时，默认表达式可以是函数引用或lambda，对照声明的函数类型精确检查（§7.4）
- enum方法**不**支持默认参数（§4.6.2）

class或struct构造函数的形参默认值在构造调用点求值，此时被构造的对象尚不存在（§4.8.6、§4.7.5）：引用`this`是编译错误——显式`this.a`与裸字段名的隐式形式都在内，两条诊断分别给出`cannot reference 'this'`与`cannot access instance member "Seed.v"`，被引用的成员在诊断中点名。默认表达式可以读取与调用静态成员，所属类内非限定名与经类型名限定都合法（§4.8.8）。

跨越`.ncu`边界的默认表达式以小表达式树的形式传递，覆盖一个可折叠的子集（§28.6）；子集之外的默认值由必须把它实体化的使用方报告为错误。在同一工程里一起编译的源码直接使用默认值。lambda默认不参与跨模块传递：库单元（无`main`入口的单元）里被导出的声明带lambda默认是编译期错误——捕获`this`或实例成员的形态由诊断点名（`a default lambda that captures 'this' or an instance member cannot be exported.`），其余导出形态报`lambda expressions are supported in default values of non-exported declarations.`；含`main`的程序单元与库内不导出的声明不受此限。

### 命名实参

*资料性：*实参可以用`name = expr`语法按参数名传递。位置实参必须出现在命名实参之前。

```nlang
int foo(int a, int b) { return a * 10 + b; }
foo(a = 5, b = 7);     // 命名，任意顺序
foo(5, b = 7);          // 混合：先位置后命名
foo(b = 7, a = 5);      // 命名，倒序
```

错误：

- `foo(b = 2, 1)` —— 命名后出现位置实参：编译错误（`positional argument cannot follow a named argument in call to "foo".`）
- `foo(1, a = 2)` —— 对`a`重复绑定：编译错误（`duplicate named argument "a" in call to "foo".`）
- `foo(c = 1)` —— 未知形参名：编译错误

构造器调用只接受位置实参——`Point(x = 1, y = 2)`是编译错误（`named arguments are not supported in constructor calls`）；见[重载解析](overloads.md)「构造器重载」。

### 引用形参

声明为`T& p`的形参指名调用方实参的存储（§4.7.9）：不做拷贝，通过`p`的写入直接落到实参上。调用点上实参必须是精确类型的可绑定存储——局部变量、形参、字段或元素；字面量与计算结果被绑定规则拒绝（§4.7.9）。`const T& p`形参只读地接受同样的实参，此外还能绑定const存储与表达式结果——后者物化为由该形参保活的临时值。

哪些形参能改调用方的变量：

| 形参形态 | 调用后调用方的变量 |
|---|---|
| `T p`（值） | 不变——形参操作的是副本 |
| `T& p`（引用） | 带着通过`p`的每一次写入 |
| `const T& p` | 不变——通过`p`的写入是编译错误 |

引用形参经虚分派与接口分派传递时与普通形参无异——引用是普通参数槽里的数据，分派机制本身对它无感知。

`out`仍是保留字（第1章），但引用形参是唯一的输出参数形态：声明里写`out T p`是编译期错误，诊断指向`T&`写法（`out parameters are retired; declare "p" as a reference formal instead.`）。
