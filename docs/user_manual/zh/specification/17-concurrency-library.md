> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 17. 并发库

并发库是两个标准库包，语义由第11章固定：`thread`包回答谁来跑、结果在哪取，`concurrent`包回答跑起来之后怎么配合。每个操作的语义——挂起、调度、失败传播、同步——都在第11章；本章固定导入形态与签名面。

## 17.1 thread包

`thread`是标准库包（§12.4）：`import thread;`把它带入作用域，每个函数都经包限定符调用（`thread.spawn(f)`）。包名是单段的（§3.3）。它的声明是标准库的`.n`源码；每个函数都是`native`函数（§7.5），其实现内建于虚拟机本身，而非随工具链发布的原生库。该包声明的句柄类——`Thread`、`Future<T>`与`Promise<T>`（§11.2、§11.5）——与函数同源，经同一次导入进入作用域。

- `thread.spawn(f)`接受一个零参函数值（§7.4）。`func<void>`委托产出`Thread`句柄；`func<T>`委托产出`Future<T>`句柄（§11.5）。带类型的形态由编译器在声明的基础形态`Thread spawn(func<void> f)`之上绑定。
- `thread.join(t)`接受一个`Thread`句柄。
- `thread.isFailed(t)`接受一个`Thread`句柄并返回`bool`；`thread.failure(t)`返回一个`Exception`（§11.1）。
- `thread.yield()`不接受参数。
- `thread.id()`返回一个`int`。
- `thread.sleep(ms)`接受一个以毫秒计的`int`。

每个函数的行为——停泊、唤醒、重新抛出失败的目标、宿主级错误——都在§11.1。

## 17.2 concurrent包

`concurrent`是标准库包（§12.4）：`import concurrent;`把它带入作用域。它的句柄类——`Mutex`、`ReentrantMutex`、`Semaphore`、`AtomicInt`、`Channel<T>`——与异常子类`ChannelClosedException`（§9.3）在包源码中声明为native类，其实现内建于虚拟机，与`thread`的函数一样；它们的构造器与方法经实例调用，而非经包限定符。`WaitGroup`是该包唯一的普通NLang类，构建于`Mutex`与`Channel`之上（§11.3）。

导入`concurrent`不会把`thread`带入作用域（§3.3）：为`WaitGroup`派生工作的代码要自己导入`thread`。
