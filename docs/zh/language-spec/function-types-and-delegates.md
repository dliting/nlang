# 函数类型与委托


`Func<R, P1, P2, ...>` 是描述**函数值**的内建泛型类型。第一个类型
实参恒为**返回类型**（可为 `void`）；其余实参按声明顺序是参数类型，
每个可选地以 `out` 前缀：

```nlang
Func<int>                 // int f()
Func<void>                // void f()
Func<int, int>            // int f(int)
Func<void, out int>       // void f(out int)
Func<int, int, out int>   // int f(int, out int)
```

签名匹配是**精确的**——没有协变/逆变，没有默认值填充。

### 函数引用

不带括号的函数名或方法名是引用——一等值，可存入局部变量、参数、
字段、数组与容器，四处传递，稍后调用：

- **自由函数引用**（`bar`）——指向该函数的静态绑定句柄。
- **绑定方法引用**（`c.foo`）——捕获接收者：对象随句柄一起走，
  其**状态跨调用保持**。
- **虚 / 接口方法引用**（`c.tw`、`ifaceVar.run`）——按名句柄，在
  每次调用时于接收者的**运行期类**上解析覆写链（晚期绑定）。

```nlang
using BinOp = Func<int, int>;

class Counter {
    int n;
    int foo(int x) { n = n + 1; return n + x; }
    virtual int tw(int x) { return x * 2; }
}
class Big : Counter {
    int tw(int x) { return x * 100; }
}
int bar(int x) { return x + 1; }
void apply(BinOp f) { io.print(f(10)); }

int main() {
    Counter c = new Counter();
    apply(bar);       // 11    自由函数
    apply(c.foo);     // 11    绑定：n 变为 1，1 + 10
    Counter b = new Big();
    apply(b.tw);      // 1000  虚分派：运行期类 Big 获胜
    BinOp g = c.foo;
    return g(1) + g(1);   // 状态保持：3 + 4 = 7
}
```

绑定发生在**消费者位点**——即存在期望函数类型的位置：声明/赋值
右值（含字段与下标存储）、返回位置、调用实参、容器方法实参
（`l.add(bar)`）、初始化列表条目（`[bar]`）。无期望函数类型的引用
是编译错误，诊断会点名该引用。

### 调用

两种调用形状：

- **裸标识符**——`f(x)`。Func 类型的局部变量/参数/字段**遮蔽**任何
  同名函数。
- **成员字段**——`obj.cb(x)`，其中 `cb` 是 Func 类型的字段。

实参按 Func 类型的签名暂存；`out` 参数在调用后写回调用者的局部
变量，并透明处理绑定方法的接收者位移。委托调用不支持具名实参。

### 相等、null、toString

- `f == g` / `f != g` 比较**句柄内容**（目标 + 接收者 + 形式）：指向
  同一函数的两个引用相等。`f == null` 与 `f != null` 合法。排序比较
  （`< <= > >=`）以及与非 Func 操作数的比较是编译错误。
- `f = null` 存入空句柄；**调用它在运行期抛出空指针异常**。在
  **null 接收者**上绑定引用（`Counter c = null; foobar(c.tw);`）在
  绑定时刻抛错。
- `f.toString()`、`f as string`、`"" + f`、`io.print(f)` 与容器格式
  化渲染 `"func <name>"`（静态句柄——含绑定的非虚方法引用）或
  `"method <name>"`（虚分派句柄）。
- **已知不一致**：`List<Func>.contains` / `indexOf` 按**恒等**比较
  元素（每个引用都是独立的堆记录），不按 `==` 的内容相等。

### 限制（编译期，具名诊断）

- 引用签名与期望 Func 类型不精确匹配的函数/方法。
- 引用带**默认参数**的函数或方法——默认值只在直接调用路径上填充。
- **枚举方法**——接收者是 int 值，不是堆对象。
- **原生方法**的任何可达形式：声明为 `native`、虚基类下的覆写、
  或接口成员的实现（原生调用没有供接收者使用的被调方栈帧）。
- **Func 类型带 `out` 参数的虚/接口引用**——运行期分派可能与编译的
  out 掩码不一致。
- **返回位置的 `out`**（`Func<out int, ...>`）——以具名诊断
  "out is only allowed on Func<...> parameters." 拒绝。
- **`new Func<...>(...)`**——不支持按名构造；请绑定引用。
- **`Dict<Func<...>, V>`**——Func 作 Dict 键（见上文恒等说明）。
- **装箱成 `Object`**（`Object o = f`）——经泛型的不兼容类型诊断
  拒绝。
- **`Func` 之外的 `void` 或 `out` 类型实参**（`List<void>`、
  `List<out int>`、`new List<out int>`）。
- **跨模块**：引用被导入的函数，或把函数引用传**给**被导入的函数
  （`.nmod` 不序列化参数签名）。
- 函数值作 `switch` 判别式（没有 case 家族能匹配）。
- 函数值不能穿过序列化 API——对持有 Func 字段的 struct/class 调
  `writeStruct` / `writeObject` 是运行期错误。
