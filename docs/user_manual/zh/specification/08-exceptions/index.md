> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 8. 异常

异常处理是结构化的：抛出的异常是一个普通对象，所在的`try`语句可以处理它，未处理的异常终结程序。`throw`语句与`try`语句的语法见§2.5。

## 8.1 抛出与传播

`throw e;`求值`e`。该值必须是`Exception`或其子类的实例（§8.3、§8.4）；其他任何类型都是编译期错误。抛出的异常是一个普通的类实例（§4.8），所以抛出形式是一次构造器调用：`throw Exception("message");`。`throw`接受一个值，而非类名。

`throw;`重新抛出当前正在处理的异常。它只在`catch`子句体的词法内部有效；其他任何地方都是编译期错误。重新抛出传播同一个异常对象：它的类、它的`message`与它的`backtrace`都不变（§8.3）。

一次抛出把控制传出正在运行的函数，传到调用方及调用链更上方、最近的带匹配`catch`子句的外层`try`语句（§8.2）；路径上的各`finally`体按§8.2的规定运行。若没有处理者匹配，宿主报告未捕获的异常，程序以退出码1终止（第9章）。报告逐环打印原因链：首行点明异常的类与消息，随后每个原因环节一行`Caused by:`（§8.3）。

```
Runtime error: Exception: save failed
Caused by: Exception: disk full
```

派生任务里未被观察的失败在运行结束时以同一格式报告（§10.9）。

语言、标准库与VM对其所报告的错误抛出§8.3的内建异常，且这些异常可捕获：经由`null`引用的访问（§4.8、§5.6、§5.7）、除以零或取余零以及越界的移位计数（§5.2）、越界的下标（§5.7）、失败的断言（§6.5）、失败的`as`向下转型（§5.5），以及第20章的库错误。

*宿主级*执行期错误（§7.1）不是异常：没有`catch`子句处理它，程序结束。定义宿主级失败的各节指明其发生位点——调用深度界（§7.1）、缺失的原生函数（§7.5）、§5.5的拆箱与`null`向下转型失败、超出成员表的枚举`toString()`（§4.6）、§11.2的`toString`嵌套界、§14.6的流失败，以及第10章的并发失败。

```nlang
throw Exception("error message");   // 抛出一个新异常
throw;                              // 重新抛出当前异常（仅在catch内）
```

*资料性：*表达式形式在编译期检查——`throw 42`是编译错误。VM抛出的运行期错误是普通异常，可被`try`/`catch`捕获：

```nlang
try {
    int x = 0;
    int y = 1 / x;           // 抛出DivByZeroException
} catch (DivByZeroException e) {
    // 被捕获
}

try {
    List<int> lst = {};
    return lst.get(999);      // 抛出IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // 被捕获
}
```

非法的`as`向下转型同样抛`InvalidCastException`，可像其他异常一样被捕获；其`message`含期望与实际类名（§4.5）。

## 8.2 try / catch / finally

语法（§2.5）允许空的catch表，但既无`catch`子句也无`finally`子句的`try`语句是编译期错误；没有`catch`的`try { } finally { }`是合法的。

`catch`子句的类型必须是`Exception`或其子类；其他任何类型都是编译期错误。一个`try`语句的各子句按声明顺序匹配。第一个类型等于所抛异常之类或其祖先的子句运行其体；其余子句被跳过。因此`catch (Exception e)`匹配每个异常，列在它之后的、更具体类的子句不可达。`catch`参数是`catch`体中一个普通的局部变量（§3.5）：可以读取也可以赋值。

每当控制离开`try`区域，`finally`体都运行，每条路径皆然：

- `try`体正常完成；
- 匹配的`catch`体完成；
- 异常展开穿过：`finally`体运行，异常继续传播——包括从`catch`体抛出的异常；
- `break`或`continue`传出该区域：`finally`体在跳转之前运行，嵌套的`try`语句内层先；
- `return`：先求值返回表达式，然后`finally`体运行，然后函数返回。

每次控制流经过，每个`finally`体恰好运行一次。嵌套的`finally`体内层先运行；它们运行之后，外层的`catch`看到在途的异常。

在`finally`体内部，`break`、`continue`、`return`与`throw`都是编译期错误：该体不得置换在途的控制流。

*资料性：*语句形状沿用其他语言熟悉的Java/C#风格：

```nlang
try {
    // 可能抛出的代码
} catch (DivByZeroException e) {
    // 处理除以零
} catch (Exception e) {
    // 处理其他任何异常
}

try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // 总是运行
}
```

## 8.3 内建异常

`Exception`扩展`Object`（§4.11）。内建异常类是`Exception`及其七个直接子类：

| 类 | 由谁抛出 |
| --- | --- |
| `Exception` | 对用户`throw`抛出`Exception`自身；对不存在的键做`Dict`读取（§4.12）；没有专属类的VM位点：越界的移位计数（§5.2）、Unicode标量值之外的`char`转换（§5.5）、以负容量构造的`Channel`（§10.4） |
| `NullPointerException` | 经由`null`的类、接口或函数引用的成员访问、方法调用或调用（§4.8、§5.6、§5.7） |
| `DivByZeroException` | 任意数值宽度上的除以零或取余零（§5.2） |
| `IndexOutOfBoundsException` | 越界的数组或`List`下标（§5.7）；带长度的数组创建中长度为负或元素数超过上限（§4.10）；越界的`string`的`substring`或`charAt`（§17.2） |
| `AssertionException` | 失败的`assert`（§6.5） |
| `InvalidCastException` | 失败的`as`向下转型（§5.5） |
| `IOException` | 失败的`io`或`fs`操作（第14章、第16章） |
| `InitializationException` | 静态初始化器经初始化协议失败时抛出，被抛对象作为其cause包裹（§4.8.8）；在初始化尚未完成时读取带初始化器的静态成员，类、接口或静态局部皆然（§4.8.8、§6.1） |

`concurrent`包（§3.9.4）声明了再一个直接子类`ChannelClosedException`，由对已关闭通道的操作抛出（§10.4）；它经`import concurrent;`像任何包类一样进入作用域，`catch`对它的处理与其他子类无异。`net`包（§3.9.4）声明了`IOException`的两个子类`SocketException`与`TimeoutException`，由第21章的套接字操作抛出；它们经`import net;`像任何包类一样进入作用域，`catch`对它们的处理与其他子类无异。`url`包（§3.9.4）声明了`IOException`的子类`UrlException`，由第22章的操作抛出；`system`包声明了`Exception`的直接子类`DateTimeException`，由`DateTime.parse`（§15.2）抛出；它们经各自的`import`像任何包类一样进入作用域，`catch`对它们的处理与其他子类无异。

每个内建异常类都是const类（§4.8.12），且每个构造器都有相同的形状：`XxxException(string message, Exception cause = null)`——第二个实参可选，用于包装原因异常。

每个异常实例携带三个只读字段，从`Exception`继承；各类都是const类，因此构造器之外的写入是编译期错误（§4.8.12）：

- `message`（`string`）——由创建该实例的构造器调用设置，或由抛出VM异常的位点设置。某节固定了文本时（§6.5），该文本是规范性的；否则消息以实现选定的措辞描述错误。用户代码可以读取该字段。
- `cause`（`Exception`）——被包装的原因异常，经构造器的第二个实参传入；未包装任何异常时为`null`。每个环节只在构造时指向前一个，因此原因链永不成环；也不存在事后改写该字段的途径。
- `backtrace`（`List<string>`，§4.12）——对VM抛出的异常，是抛出时拍摄的调用栈快照：每帧一个条目，最内层在前，每个条目形如`funcName (file:line)`。自由函数以模块限定名命名；方法以裸方法名命名。`file`是模块编译时记录的源码路径；行号未知的帧渲染为`?`；没有记录源码路径的模块渲染其模块名并带`.n`后缀。用户代码创建的异常以空backtrace开始。读取该字段返回一个副本（§4.8.12）：对副本做`add`或下标写入永远不会触及异常本身，且两次读取得到不同的副本。

*资料性：*各字段在构造器之外只读：

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();        // > 0 —— VM设置了消息
    int frames = e.backtrace.length(); // >= 1 —— VM快照了栈
}

// 原因链：把底层失败包装进更高层的异常。
Exception root = Exception("disk full");
Exception wrap = Exception("save failed", root);
if (wrap.cause != null) {
    string rootMsg = wrap.cause.message;   // "disk full"
}
```

## 8.4 用户异常类

用户可以定义类扩展`Exception`（§4.8.5）以携带额外状态。`Exception`家族是const的，因此用户异常类必须声明为`const class`：const类只能被另一个const类继承（§4.8.12）。其构造器用`super(msg)`或`super(msg, cause)`转发消息（§4.8.6）；没有声明构造器时，合成的零参构造器让继承的字段保持其默认值（§4.8.3，§4.8.6）。类自身的字段在对象布局中排在继承字段之后（§4.8.5），且只能在子类自己的构造器内赋值（§4.8.12）。

用户异常的抛出与处理和内建异常完全一样：`catch (MyException e)`在§8.2的规则下匹配`MyException`及其子类，而`catch (Exception e)`同样匹配它。

```nlang
const class MyException : Exception {
    int code;
    public MyException(string msg, int c) {
        super(msg);          // 转发到Exception(message, cause = null)构造
        this.code = c;       // 自身字段在构造器内赋值
    }
}
```

### 调试异常

*资料性：*在nide中按F5启动调试会话，打开「在异常处中断」开关可在抛出点暂停（见[nide中的调试](../../getting-started/debugging.md)）；在命令行上，`ndb`的`catch on`同样在抛出点中断（见[命令行ndb](../../cli-tools/ndb.md)）。
