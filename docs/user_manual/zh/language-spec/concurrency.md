# 并发

NLang提供轻量并发：`thread`包派生**虚拟线程**——共享载体池的执行上下文，存储操作按域加锁保护，不是各自独立的操作系统线程。`Mutex`与`Channel<T>`提供互斥与传递式同步；`Future<T>`取回派生任务的返回值；`join`建立完成顺序；标准库`concurrent`包在其上提供`WaitGroup`与`Semaphore`。`thread`是标准库包，声明见`stdlib/thread.n`，与其他标准库包一样需`import thread;`后使用；`Mutex`、`Channel<T>`、`Future<T>`等内建类无需导入。

调度的确定性分档：单载体（库默认）下完全确定——严格先进先出（FIFO，first in first out）轮转，每上下文跑满固定回边配额才让位（仅当另有可运行上下文时），同一程序在同样的输入下产生同样的交错顺序；多载体下出队带唤醒亲和，交错顺序未指定（见[GIL与多载体](concurrency-gil-carriers.md)）。`sleep`按截止时间排序唤醒。单载体测试可断言交错顺序与终值，多载体测试只断言同步后的结果——都不断言时长。

## 并发模型：阻塞式书写，无异步着色

NLang属于**虚拟线程**一族（同Go、Java虚拟线程）：写并发代码就像写普通顺序代码——遇到等待就"停下让位"，调度器在挂起点之间交错推进各个虚拟线程。这套模型下"单线程的异步"就是默认形态：库默认只有一个载体，`thread.spawn`出的多个任务在一条执行线上轮流跑，等定时器、等通道、等彼此，与JavaScript事件循环解决的问题相同，只是写法是阻塞式直书：

| 单线程异步（JavaScript）写法 | NLang对应写法 |
|---|---|
| 事件循环 | 调度器运行队列（默认单载体） |
| `setTimeout(f, 100)` | `thread.spawn(f)`＋`thread.sleep(100)` |
| `await promise`／`Promise.all` | `f.get()`／`thread.join(t)` |
| 回调嵌套 | 直接顺序书写 |

因此NLang没有也不需要`await`关键字：函数不做异步着色，没有状态机变换；`f.get()`就是诚实的阻塞式等待——停泊当前虚拟线程，完成值就绪后原地续行。要到多个载体上真并行（纯计算段与对象字段访问直接重叠，容器、字符串、流、同步原语各归各的域锁），用`SetCarrierThreadCount(n)`显式选入（见[GIL与多载体](concurrency-gil-carriers.md)）。

## 选型导引

从你要做的事出发，而不是从工具清单出发。安装包自带的`examples/conc_*.n`把下表每一行都走了一遍：

| 你想… | 用 |
|---|---|
| 启动一个后台任务 | `thread.spawn(f)` |
| 取回任务的返回值 | 派生带返回值的函数 → `Future<T>` → `get()` |
| 在任务之间传值 | `Channel<T>`（send/recv/close） |
| 保护共享可变状态 | `Mutex`（lock/unlock/tryLock） |
| 限制同时进入某段代码的任务数 | `Semaphore(n)` |
| 等一批任务全部完成 | `WaitGroup`（add/done/wait）或逐个`join` |
| 一次性叫醒一组等待者 | `Channel.close()`广播 |

## 去哪里查

- [GIL与多载体](concurrency-gil-carriers.md)——载体、存储域锁、阻塞式原生调用窗口、唤醒亲和，以及各确定性分档的保证
- [thread包](concurrency-thread.md)——`spawn`、`join`、`yield`、`id`、`sleep`
- [Future](concurrency-future.md)——取回派生任务的返回值
- [Mutex](concurrency-mutex.md)——不可重入互斥
- [Channel](concurrency-channel.md)——消息传递与关闭语义
- [标准库同步工具](concurrency-concurrent.md)——`WaitGroup`与`Semaphore`
- [停泊与调度限制](concurrency-parking.md)——跨停泊的异常、主退出策略、死锁报告、嵌套求值窗口
