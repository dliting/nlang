> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 17. 并发库

并发库是建立在第11章内核原语之上的两个标准库包：`thread`包驱动执行上下文，`concurrent`包提供`WaitGroup`与`Semaphore`。每个操作的语义——挂起、调度、失败传播、同步——都在第11章；本章固定导入形态与签名面。

## 17.1 thread包

`thread`是标准库包（§12.4）：`import thread;`把它带入作用域，每个函数都经包限定符调用（`thread.spawn(f)`）。包名是单段的（§3.3）。它的声明是标准库的`.n`源码；每个函数都是`native`函数（§7.5），其实现内建于虚拟机本身，而非随工具链发布的原生库。该包产出的句柄类——`Thread`与`Future<T>`——是内建句柄类（§11.2）：它们的名字无需导入。

- `thread.spawn(f)`接受一个零参函数值（§7.4）。`func<void>`委托产出`Thread`句柄；`func<T>`委托产出`Future<T>`句柄（§11.5）。带类型的形态由编译器在声明的基础形态`Thread spawn(func<void> f)`之上绑定。
- `thread.join(t)`接受一个`Thread`句柄。
- `thread.yield()`不接受参数。
- `thread.id()`返回一个`int`。
- `thread.sleep(ms)`接受一个以毫秒计的`int`。

每个函数的行为——停泊、唤醒、重新抛出失败的目标、宿主级错误——都在§11.1。

## 17.2 concurrent包

`concurrent`是标准库包（§12.4）：`import concurrent;`把它带入作用域。与`thread`不同，它没有原生支撑：它的两个类是在包源码中声明、建立在内核原语（`thread`、`Mutex`、`Channel`）之上的普通NLang类。因此它们的名字需要导入，与§11.2的内建句柄类不同。

- `WaitGroup()`构造一个没有未结工作的计数器。`add(n)`累计`n`个未结单位，`done()`注销一个单位，`wait()`停泊直到计数到达零。契约及其失败模式在§11.3。
- `Semaphore(initial)`设置初始许可数。`acquire()`取一个许可，`release()`归还一个。准入规则在§11.3。

导入`concurrent`不会把`thread`带入作用域（§3.3）：为`WaitGroup`派生工作的代码要自己导入`thread`。
