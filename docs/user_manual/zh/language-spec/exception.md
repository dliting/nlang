# 异常


NLang 支持带 Java/C# 风格类层次的结构化异常处理。所有异常都是 `Exception`
或其子类的实例。

### 内建异常类

| 类                          | 父类        | 抛出方 |
|------------------------------|-------------|--------|
| `Exception`                  | `Object`    | 用户 `throw` / Dict 键未找到 |
| `NullPointerException`       | `Exception` | 空引用访问 |
| `DivByZeroException`          | `Exception` | 整数除/取模零 |
| `IndexOutOfBoundsException`  | `Exception` | 数组/列表索引越界 |
| `AssertionException`          | `Exception` | `assert(false)` |

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

- 支持多个 `catch` 子句，按声明顺序匹配。
- 第一个匹配的 catch 子句执行；其余被跳过。
- `catch (Exception e)` 捕获所有异常（Exception 是基类）。
- catch 变量 `e` 是 catch 语句体内的普通局部变量。

### `throw`

```nlang
throw new Exception("error message");   // 抛出一个新异常
throw;                                   // 重新抛出当前异常（仅在 catch 内）
```

- `throw expr` —— 表达式必须求值为 Exception 子类实例。抛出非 Exception 值
  （例如 `throw 42`）是编译错误。
- `throw;`（重抛）仅在 `catch` 语句体词法内合法。在 catch 块外使用是编译错
  误。

### 用户定义的异常子类

```nlang
class MyException : Exception {
    int code;
    public int MyException(string msg) {
        super(msg);          // 转发到 Exception(message) 构造
        this.code = 42;
        return 0;
    }
}
```

用户类可以扩展 `Exception` 以携带额外字段。若没有用户构造函数，则使用默认
构造函数，继承的字段零初始化；用户构造函数通常经 `super(msg)` 转发消息（
见 [类](class.md)「super() —— 构造函数链」）。

### 异常字段

异常实例暴露两个可读可写字段：

- `message`（string）—— 异常消息。由构造函数（`new Exception("msg")`）或 VM
  错误点设置。用户代码可写。
- `backtrace`（List&lt;string&gt;）—— VM 抛出的异常的抛出时刻调用栈快照
  （`funcName.n:line` 条目，最内层在前）。用户构造的异常初始 backtrace 为
  空。

```nlang
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();       // > 0 —— VM 设置了消息
    int frames = e.backtrace.length(); // >= 1 —— VM 快照了栈
}

//User 子类继承这两个字段；自身字段排在它们之后。
class MyException : Exception {
    int code;
}
MyException e = new MyException();
e.message = "custom";   // 可写
e.code = 42;
```

### VM 错误可被捕获

运行期错误（空指针异常（NPE，null pointer exception）、除以零、数组/列表索引越界、断言失败）抛出对应的
Exception 子类，可被 `try/catch` 捕获：

```nlang
try {
    int x = 0;
    int y = 1 / x;           // 抛出 DivByZeroException
} catch (DivByZeroException e) {
    // 被捕获
}

try {
    List<int> lst = new List<int>();
    return lst.get(999);      // 抛出 IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // 被捕获
}
```

**未捕获异常**沿调用栈向上传播。若找不到处理器，程序以退出码 1 终止。

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

`finally` 具有完整 Java 语义——当控制以任何这些路径离开 try 区域时，finally
语句体运行：

- try 语句体正常完成（包括从底部落出）
- 某个 catch 子句完成（无论是否匹配）
- 异常展开穿过（finally 处理器运行其语句体拷贝，然后重新抛出原异常；这
  包括从 catch 语句体内部抛出的异常）
- `break` / `continue` 转移出该区域（跳转前运行内联的 finally 语句体拷贝，
  嵌套 try 时最内层优先）
- `return`（返回表达式**先**求值，然后 finally 语句体运行，然后函数返回）

嵌套：内层 finally 语句体先于外层运行；之后，包围的 catch（若存在）看到该
异常。每个 finally 语句体每次控制流通过恰好执行一次——正常路径与异常路径的
拷贝是不相交的代码区域。

`try { } finally { }` 不带任何 catch 子句是合法的（finally 条目是唯一的处理
器）。

**限制**：`break`、`continue`、`return`、`throw` 不允许*出现在 finally 语句
体内*（编译错误）。finally 语句体不得吞掉进行中的控制流或异常。

### 调试异常

在 nide 中按 F5 启动调试会话；打开"在异常处中断"开关可在抛出点暂停（见
[nide 中的调试](../getting-started/debugging.md)）；在命令行上，`ndb` 的
`catch on` 同样在抛出点中断（见 [命令行 ndb](../cli-tools/ndb.md)）。
