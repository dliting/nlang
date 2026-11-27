> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 7.4 函数值与委托

函数类型写作`func<R, P1, ..., Pn>`：第一个类型实参是返回类型——函数不返回值时为`void`——其余实参按顺序是参数类型（§2.3）。参数与返回类型都可以携带`&`标记（`func<int&, int&>`——引用形参与引用返回，§4.7.9）。无参数的函数写作`func<R>`。

`void`只作为`func<...>`返回槽的类型实参出现；任何其他写作`void`的类型实参——如`List<void>`——都是编译期错误。

两个函数类型是同一类型，当且仅当它们的返回类型、参数类型与`&`标记逐位置一致。没有变异性：返回类型与参数类型都不允许不同，即使差异只是一次隐式转换也不行（§4.4）。带`&`的函数类型与其去标记形态是不同类型，把无标记函数绑给带标记期望（或反之）是编译错误（`function "plain" does not match the signature of "func<Int32, Int32&>"`——诊断拼写逐位置带标记）。

带`&`标记的函数值照常引用与调用：调用时`&`实参位置走与直接调用相同的可绑定存储规则（§7.2），通过函数值的写入落到调用方；`&`返回把被调方的引用原样传给绑定侧。

函数类型的值指代一个函数：自由函数、静态方法，或绑定到某个实例的方法（§5.9）。泛型函数也可被引用——其值指代该函数的一个实例化：类型实参由期望函数类型推断，或经显式实例化拼写点名（`Tag<int>.tag`，§4.12）。默认值是`null`；调用`null`函数值是执行期错误。函数值是引用类型，其唯一的隐式转换是到`string`（§4.4）：它不进入`Object`。

引用形式——自由函数、经类型名引用的静态方法、绑定到接收者的实例方法，以及每次调用时迟绑定的虚方法或接口方法——见§5.9。

函数引用只在期望函数类型的地方形成：局部声明、初始化器、赋值、返回、实参、数组元素或花括号条目（§5.8）。构造调用的实参是例外：在那里直接形成的引用是编译期错误，须先把引用绑给`func`局部再传这个局部（见限制节）。没有期望类型时，该引用是编译期错误。所指代的签名必须与期望的函数类型完全一致：没有变异性，参数与结果都不做转换。参数带默认值的函数不能被引用：默认值只在直接调用处填充（§7.2）。枚举方法与原生实例方法不能被引用。

绑定引用在值形成时捕获其接收者；经由该引用做的状态改变持续留在接收者对象中。虚方法或接口引用在每次调用时按接收者的运行时类分派（§5.9，§4.8.9，§4.9.4）。

函数值以`v(a1, ..., an)`调用（§5.6）；函数类型的字段以`obj.cb(a1, ..., an)`调用。函数类型的局部遮蔽同名的函数（§3.7）。经由函数值的调用不接受命名实参（§7.2）。调用null函数值是可在执行期捕获的错误（§5.6）；在null接收者上形成绑定引用，在形成处抛出可捕获的执行期错误。

两个函数值在`==`下相等，当它们指代同一函数的同一实例化——泛型函数的实例化参与相等，同函数不同实例化不相等——且对绑定引用而言携带相等的接收者；闭包按闭包身份比较（同一次激活的同一lambda字面量相等，见下文Lambda表达式节）；`!=`是补集，`== null`是允许的。排序比较是编译期错误。函数值经由`toString`渲染（§5.3，§4.11.2）。

函数值不进入`Object`。它不能作`Dict`的键、`switch`的判别式，也不能作序列化方法的目标（第14章）。

### 函数类型语法

*资料性：*`func`关键字引入函数类型：第一个类型实参恒为返回类型，其余实参按声明顺序是参数类型。

```nlang
func<int>                  // int f()
func<void>                 // void f()
func<int, int>             // int f(int)
func<int, int&>            // int f(int&)
func<int&, int&>           // int& f(int&)
```

### 函数引用

*资料性：*不带括号的函数名或方法名是引用——一等值，可存入局部变量、参数、字段、数组与容器，四处传递，稍后调用：

- **自由函数引用**（`bar`）——指向该函数的静态绑定句柄。
- **静态方法引用**（`Util.twice`）——指向静态方法的无接收者句柄；在所属类内也可用非限定名。
- **绑定方法引用**（`c.foo`）——捕获接收者：对象随句柄一起走，其**状态跨调用保持**。
- **虚 / 接口方法引用**（`c.tw`、`ifaceVar.run`）——句柄持有方法名，在每次调用时于接收者的**运行期类**上解析覆写链（晚期绑定）。

```nlang
import io;
using BinOp = func<int, int>;

class Counter {
    int n;
    public static int twice(int x) { return x * 2; }
    public int foo(int x) { n = n + 1; return n + x; }
    public virtual int tw(int x) { return x * 2; }
}
class Big : Counter {
    public int tw(int x) { return x * 100; }
}
int bar(int x) { return x + 1; }
void apply(BinOp f) { io.print(f(10)); }

int main() {
    Counter c = Counter();
    apply(bar);       // 11自由函数
    apply(Counter.twice); // 20静态：无接收者
    apply(c.foo);     // 11绑定：n变为1，1 + 10
    Counter b = Big();
    apply(b.tw);      // 1000虚分派：运行期类Big获胜
    BinOp g = c.foo;
    return g(1) + g(1);   // 状态保持：3 + 4 = 7
}
```

绑定发生在**使用方位置**——即存在期望函数类型的位置：声明/赋值右值（含字段与下标存储）、返回位置、调用实参、容器方法实参（`l.add(bar)`）、初始化列表条目（`[bar]`）。构造调用的实参是例外：那里要传`func`局部，而不是裸名字（见限制节）。无期望函数类型的引用是编译错误，诊断会点名该引用。

### Lambda表达式

*资料性：*`->`引入lambda表达式——就地写出的函数值。lambda与函数引用共享同一组使用方位置：声明、赋值、返回位置、调用实参、数组元素、花括号条目——构造调用的实参又是例外：lambda须先落到`func`局部（见限制节）。每个lambda都对照期望函数类型检查；没有期望函数类型的lambda是编译期错误。

**初识lambda。**参数表点名参数；箭头`->`把它与体分开。表达式体的值即返回值；块体是花括号内的语句序列，经`return`返回——`func<void>`期望下的块体无需`return`，其余期望下块体不返回值是编译错误。

```nlang
import io;

int main() {
    func<int> seven = () -> 7;
    func<int, int> addOne = (x) -> x + 1;
    func<int, int, int> add = (x, y) -> x + y;
    func<void> hello = () -> { io.print("hello"); };
    hello();                     // hello
    return add(20, addOne(1));   // 22
}
```

**参数表。**条目要么是裸标识符——类型与`&`标记取自期望函数类型的对应位置——要么是带类型的标识符（`(int x)`、`(Point p)`、`(int& x)`）：标注按普通类型书写——基本类型、类名、内建泛型实例化如`List<int>`、数组类型如`int[]`——`&`标记可有可无，且必须与期望类型逐位置精确一致，无变异性。裸条目与带类型条目可以混用。空表`()`写出零参lambda。

```nlang
int main() {
    func<int, int, int> add = (int x, int y) -> x + y;
    func<string, string> shout = (string s) -> s + "!";
    func<int, int> doubled = (x) -> x * 2;           // 裸条目
    func<int, int, int> mixed = (x, int y) -> x + y; // 混用条目
    if (shout("go") != "go!") { return 1; }
    return add(2, 3) * 100 + doubled(21) * 10 + mixed(5, 6);  // 931
}
```

参数个数与期望不符被拒绝（"the lambda takes 2 parameters; the expected type … expects 1"），显式参数类型与期望不符同样被拒绝（"lambda parameter … does not match the expected type …"）。lambda参数不能带默认值，lambda也没有泛型参数表。

**捕获。**lambda体内可见的外层局部变量（含形参）被引用即捕获：该变量的存储移入堆上，lambda内外共享同一存储，一边的写入对另一边可见。同一变量的多个捕获点与外层读写都落在同一存储上。

```nlang
int main() {
    int count = 0;
    func<int> bump = () -> { count = count + 1; return count; };
    func<int> peek = () -> count;
    bump();                   // count = 1
    bump();                   // count = 2
    return peek();            // 2——两个lambda共享同一存储
}
```

捕获不改变被捕获变量的声明语义：`const`局部捕获后在lambda内赋值仍是编译错误。`static`局部不被捕获——lambda体内引用它直接寻址静态槽，与所有调用共享（§6.1）。lambda体内引用外层`static`局部合法，声明新的`static`局部仍受§6.1的边界约束（函数或方法体内可声明，泛型上下文内不可）。`catch`子句的变量在此意义下是普通局部——处理器内的lambda捕获该子句命名的异常对象。lambda体内访问实例成员即捕获接收者；静态上下文中引用`this`是编译错误。`&`引用（引用局部、引用形参、`foreach`的引用头）捕获的是绑定本身：lambda内的读写穿透到被引方存储，被引方生命周期的安全由语言保证——每个`&`绑定的目标都是堆上存储。

**循环头新鲜。**`for`与`foreach`头部声明的变量，每次迭代是一个新变量：捕获时各迭代的lambda各自持有当次值，不共享循环结束后的尾值。

```nlang
int main() {
    List<func<int>> funcs = List<func<int>>();
    for (int i = 0; i < 3; i = i + 1) {
        funcs.add(() -> i);
    }
    func<int> second = funcs.get(1);
    return second();          // 1——每次迭代捕获自己的i
}
```

**引用形参与引用返回。**`func<int&, int&>`期望下的lambda可声明`&`形参、`&`返回：`&`形参的写入穿透到调用方存储；返回被捕获变量的引用合法——其存储已在堆上，调用结束后依然有效。

```nlang
int main() {
    func<void, int&> setter = (int& x) -> { x = 42; };
    int v = 0;
    setter(v);                // 写入穿透到v
    func<int&, int&> id = (int& x) -> x;
    int& r = id(v);           // r是v的别名
    r = 8;
    return v;                 // 8
}
```

**递归。**lambda可经它绑定的局部递归调用自己：该局部在lambda创建前已捕获，体内读取它即读取被捕获存储。

```nlang
int main() {
    func<int, int> fact = (n) -> { if (n <= 1) { return 1; } return n * fact(n - 1); };
    return fact(5);           // 120
}
```

**返回函数的函数。**表达式体本身可以是lambda，于是一个字面量写出柯里化函数：外层lambda返回内层，内层捕获外层的参数。

```nlang
int main() {
    func<func<int, int>, int> add = (x) -> (y) -> x + y;
    func<int, int> add3 = add(3);   // 内层lambda持有x = 3
    return add3(4);                 // 7
}
```

调用返回的函数值要经名字进行：`add(3)(4)`这样的链式调用不在表达式文法内。返回lambda的函数让每次调用获得自己的捕获存储——相互独立的计数器工厂：

```nlang
func<int> makeCounter() {
    int n = 0;
    return () -> { n = n + 1; return n; };
}

int main() {
    func<int> c = makeCounter();
    c();                      // 1
    c();                      // 2
    return c();               // 3
}
```

**泛型上下文。**泛型函数或泛型类方法体内的lambda可捕获类型参数类型的变量：捕获的存储携带真实的运行期实例化，lambda体内的`T`操作按实际类型分派——`typeName(T)`与`==`比较与泛型函数体外行为一致。

**闭包身份。**闭包值由lambda方法与其捕获存储构成。同一次作用域激活内对同一lambda字面量求值两次得到**相等**的函数值；重新激活作用域——再次调用包含它的函数、进入循环的下一迭代——得到**不相等**的闭包。零捕获lambda同规。因此「不相等」断言要用两次激活的闭包，同激活的两次求值恒相等。

```nlang
func<int> makeTick() {
    int n = 0;
    return () -> { n = n + 1; return n; };
}

int main() {
    func<int> p = makeTick();
    func<int> q = makeTick();
    if (p != q) { return 5; }  // 两次激活：闭包不相等
    return 1;                  // 两次激活仍相等即断言失败
}
```

闭包的`toString`渲染为`"lambda"`；`==`/`!=`与`null`规则同函数引用（见下文）。`thread.spawn`接受零参lambda作为派生目标（§10.1）。

### 调用

*资料性：*三种调用形状：

- **非限定标识符**——`f(x)`。`func`类型的局部变量/参数/字段**遮蔽**任何同名函数。
- **成员字段**——`obj.cb(x)`，其中`cb`是`func`类型的字段。
- **静态字段**——`Cfg.dbl(x)`，其中`dbl`是`func`类型的静态字段；经类型名限定，无接收者，调用字段中当前的句柄。

实参按`func`类型的签名暂存；绑定方法的接收者由调用自动处理。委托调用不支持具名实参。

### 相等、null、toString

*资料性：*`f == g` / `f != g`比较**句柄内容**（目标 + 接收者 + 形式 + 实例化描述符）：指向同一函数同一实例化的两个引用相等。`f == null`与`f != null`合法。排序比较（`< <= > >=`）以及与`func`之外操作数的比较是编译错误。

- `f = null`存入空句柄；**调用它在运行期抛出空指针异常**。在**null接收者**上绑定引用（`Counter c = null; register(c.tw);`）在绑定时刻抛错。
- `f.toString()`、`f as string`、`"" + f`、`io.print(f)`与容器格式化产生的字符串形式为`"func <name>"`（静态句柄——含绑定的非虚方法引用）、`"method <name>"`（虚分派句柄）或`"lambda"`（闭包）。
- `List<func<...>>`的`contains` / `indexOf`按`==`的相等性比较元素——两个指向同一函数同一实例化的独立引用算匹配（§12.1）。

### 限制

*资料性：*下列每一项都是编译期错误：

- 引用签名与期望`func`类型不精确匹配的函数/方法。
- 引用带**默认参数**的函数或方法——默认值只在直接调用路径上填充。
- **带默认值的`func`形参**（如`void apply(func<int, int> g = plus1)`）——声明本身被拒：声明侧默认表达式在该位置没有期望函数类型，因此没有任何函数能声明这样的默认值。
- **静态方法引用**——静态方法经类型名（`Util.twice`）绑定为函数值，含继承的静态方法、接口静态方法、native静态方法，以及所属类内的非限定名形态；经对象接收者（`b.twice`）仍被拒（"the static method must be called through the type name"）。
- **经类型名的实例方法**——被拒（"the instance method must be called through a receiver object"）。
- **枚举方法**——接收者是int值，不是堆对象。
- **native实例方法**的任何可达形式：声明为`native`、虚基类下的覆写、或接口成员的实现（native调用没有供接收者使用的被调方栈帧）。native静态方法可经类型名引用。
- **类型实参中任何位置的`out`类型标记**（`func<out int, ...>`、`List<out int>`）——报出编译错误"the out type marker is retired; use the reference mark instead."并拒绝。
- **`func<...>(...)`**——不支持以名称构造；请绑定引用。
- **`Dict<func<...>, V>`**——`func`作Dict键（见上文恒等说明）。
- **装箱成`Object`**（`Object o = f`，见[Object与装箱](../04-types/object.md)）——经泛型的不兼容类型诊断拒绝。
- **`func`之外的`void`类型实参**（`List<void>`）。
- 函数值作`switch`判别式（没有case家族能匹配）。
- 函数值不能穿过序列化方法——对持有`func`字段的struct/class调`writeStruct` / `writeObject`是运行期错误（第14章）。
- **lambda没有期望函数类型**——不在任何使用方位置上的lambda字面量。
- **lambda字面量或裸函数引用作构造调用实参**（`Handler((x) -> x + 1)`、`Handler(plus1)`）——构造调用实参位置不携带期望函数类型，须先把值绑给`func`局部再传局部。修法排期见`docs/roadmap.md`的远期特性清单。
- **lambda参数表条目不是参数形状**（`invalid lambda parameter list`——裸表达式混入参数表）。
- **lambda参数带默认值**——文法没有默认值位（`a lambda parameter cannot have a default value`）。
- **期望`func<void>`而表达式体产出值**；期望非`void`而块体没有`return`。
- **lambda的显式参数类型与期望不一致**——逐位置精确一致，无变异性。
- **lambda带泛型参数表**（`<T>`形参）。
- **lambda体内引用未声明名字**——与普通函数体相同的名字解析规则。
- **lambda作实例字段初始化器**——实例字段初始化器是常量表达式位置。静态字段初始化器可持有lambda，类的静态初始化器在首次使用时求值一次。
- **静态上下文的lambda引用`this`**（§6.1）。
