# 异常


NLang支持带Java/C#风格类层次的结构化异常处理。所有异常都是`Exception`或其子类的实例。

### 内建异常类

| 类                          | 父类        | 抛出方 |
|------------------------------|-------------|--------|
| `Exception`                  | `Object`    | 用户`throw` / Dict键未找到 |
| `NullPointerException`       | `Exception` | 空引用访问 |
| `DivByZeroException`          | `Exception` | 整数除/取模零 |
| `IndexOutOfBoundsException`  | `Exception` | 数组/列表索引越界 |
| `AssertionException`          | `Exception` | `assert(false)` |
| `InvalidCastException`        | `Exception` | `as`强转失败（实际类型不在目标类型的超类链上） |
| `IOException`                 | `Exception` | 文件读写失败 |
| `ChannelClosedException`      | `Exception` | 对已关闭通道的`send`/`recv`；由`concurrent`包声明，随`import concurrent;`进入作用域（见[通道](concurrency-channel.md)） |

所有内建异常类都是const类（见[类](class.md)「const类与const结构体」），构造函数统一为`XxxException(string message, Exception cause = null)`——第二实参可选，用于包装原因异常（见下文「异常字段」）。

### `try` / `catch`

```nlang
try {
    // 可能抛出的代码
} catch (DivByZeroException e) {
    // 处理除以零
} catch (Exception e) {
    // 处理其他任何异常
}
```

- 支持多个`catch`子句，按声明顺序匹配。
- 第一个匹配的catch子句执行；其余被跳过。
- `catch (Exception e)`捕获所有异常（Exception是基类）。
- catch变量`e`是catch语句体内的普通局部变量。

### `throw`

```nlang
throw Exception("error message");   // 抛出一个新异常
throw;                                   // 重新抛出当前异常（仅在catch内）
```

- `throw expr` —— 表达式必须求值为Exception子类实例。抛出非Exception值（例如`throw 42`）是编译错误。
- `throw;`（重抛）仅在`catch`语句体词法内合法。在catch块外使用是编译错误。

### 用户定义的异常子类

```nlang
const class MyException : Exception {
    int code;
    public MyException(string msg, int c) {
        super(msg);          // 转发到Exception(message, cause = null)
        this.code = c;       // 自有字段在构造函数里赋值
    }
}
```

用户类可以扩展`Exception`以携带额外字段。Exception家族是const类，用户子类也必须声明为`const class`——const类只能被const类继承（见[类](class.md)「const类与const结构体」）。若没有用户构造函数，则使用默认构造函数，继承的字段零初始化。额外字段只能在子类自己的构造函数里赋值；继承的基类字段不可在子类构造函数里改写，补充数据经`super(msg)`或`super(msg, cause)`转发到基类构造函数（见[类](class.md)「super() —— 构造函数链」）。

### 异常字段

异常实例暴露三个只读字段——异常类是const类，字段绑定构造后不可写，构造函数之外的写入是编译错误（`"message" is immutable`）：

- `message`（string）—— 异常消息。由构造函数（`Exception("msg")`）或VM错误点设置。
- `cause`（Exception）——被包装的原因异常，构造时经第二实参传入，未包装时为null。每一环只在构造时指向前一环，因此链不可能成环；NLang不提供`initCause`式的事后设置。
- `backtrace`（List&lt;string&gt;）——VM抛出异常那一刻的调用栈快照（`funcName.n:line`条目，最内层在前）。用户构造的异常初始backtrace为空。

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();        // > 0 —— VM设置了消息
    int frames = e.backtrace.length(); // >= 1 —— VM快照了栈
}

//原因链：把底层失败包装进高层异常
Exception root = Exception("disk full");
Exception wrap = Exception("save failed", root);
if (wrap.cause != null) {
    string rootMsg = wrap.cause.message;   // "disk full"
}
```

子类继承这三个字段；自身字段（如`code`）排在cause之后，只能在子类构造函数里赋值。`List`/`Dict`字段（如`backtrace`）经成员读取返回一份**副本**（const类容器字段的读取规则，见[类](class.md)「const类与const结构体」）——对副本`add`或下标写入不影响异常自身；两次读取得到不同副本，副本之间的`==`比较没有意义。

### VM错误可被捕获

运行期错误——空指针异常（NPE，null pointer exception）、除以零、数组/列表索引越界、断言失败——抛出对应的Exception子类，可被`try/catch`捕获：

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

非法的`as`类向下强转同样抛`InvalidCastException`，可被`catch`捕获，`message`含期望/实际类叶名（见[类型强制转换](type-casts.md)）。

**未捕获异常**沿调用栈向上传播。若找不到处理器，程序以退出码1终止，错误报告逐环打印原因链：

```
Runtime error: Exception: save failed
Caused by: Exception: disk full
```

spawned任务里未被观察的失败在运行结束时以同一格式报告（`failed with uncaught ... (never joined)`行后跟`Caused by:`行，见[线程](concurrency-thread.md)）。

### `finally`

```nlang
try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // 总是运行
}
```

`finally`具有完整Java语义——当控制以任何这些路径离开try区域时，finally语句体运行：

- try语句体正常完成（包括从底部落出）
- 某个catch子句完成（无论是否匹配）
- 异常传播穿过（finally语句体运行，然后原异常重新抛出；这包括从catch语句体内部抛出的异常）
- `break` / `continue`转移出该区域（跳转前运行finally语句体，嵌套try时最内层优先）
- `return`（返回表达式**先**求值，然后finally语句体运行，然后函数返回）

嵌套：内层finally语句体先于外层运行；之后，包围的catch（若存在）看到该异常。每个finally语句体每次控制流通过恰好执行一次。

`try { } finally { }`不带任何catch子句是合法的（finally子句是唯一的处理器）。

**限制**：`break`、`continue`、`return`、`throw`不允许*出现在finally语句体内*（编译错误）。finally语句体不得吞掉进行中的控制流或异常。

### 调试异常

在nide中按F5启动调试会话；打开「在异常处中断」开关可在抛出点暂停（见[nide中的调试](../getting-started/debugging.md)）；在命令行上，`ndb`的`catch on`同样在抛出点中断（见[命令行ndb](../cli-tools/ndb.md)）。
