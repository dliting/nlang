# 并发

NLang提供轻量并发：`thread`包派生**虚拟线程**——共享载体池、在全局解释器锁下每次只运行一个的执行上下文，不是各自独立的操作系统线程。`Mutex`与`Channel<T>`提供互斥与传递式同步；`Future<T>`取回派生任务的返回值；`join`建立完成顺序；标准库`concurrent`包在其上提供`WaitGroup`与`Semaphore`。`thread`是标准库包，声明见`stdlib/thread.n`，与其他标准库包一样需`import thread;`后使用；`Mutex`、`Channel<T>`、`Future<T>`等内建类无需导入。

调度是完全确定性的：载体池按先进先出（FIFO，first in first out）队列轮转上下文，每上下文跑满固定回边配额才让位（仅当另有可运行上下文时）；`sleep`按截止时间排序唤醒。同一程序在同样的输入下产生同样的交错顺序——测试只断言顺序与终值，不断言时长。

## 并发模型：阻塞式书写，无异步着色

NLang属于**虚拟线程**一族（同Go、Java虚拟线程）：写并发代码就像写普通顺序代码——遇到等待就"停下让位"，调度器在挂起点之间交错推进各个虚拟线程。这套模型下"单线程的异步"就是默认形态：库默认只有一个载体，`thread.spawn`出的多个任务在一条执行线上轮流跑，等定时器、等通道、等彼此，与JavaScript事件循环解决的问题相同，只是写法是阻塞式直书：

| 单线程异步（JavaScript）写法 | NLang对应写法 |
|---|---|
| 事件循环 | 调度器运行队列（默认单载体） |
| `setTimeout(f, 100)` | `thread.spawn(f)`＋`thread.sleep(100)` |
| `await promise`／`Promise.all` | `f.get()`／`thread.join(t)` |
| 回调嵌套 | 直接顺序书写 |

因此NLang没有也不需要`await`关键字：函数不做异步着色，没有状态机变换；`f.get()`就是诚实的阻塞式等待——停泊当前虚拟线程，完成值就绪后原地续行。要到多个载体上真并行（受全局解释器锁约束，原生阻塞窗口除外），用`SetCarrierThreadCount(n)`显式选入（见[GIL与多载体](#gil与多载体)）。

## GIL与多载体

虚拟机持有**全局解释器锁**（GIL，global interpreter lock）：任意时刻恰有一个虚拟线程在执行字节码，上下文切换只发生在安全点（函数入口与循环回边），并受每约1000个回边一次的调度预算约束。字节码层的互斥由GIL保证，语言代码无需担心两个虚拟线程同时改写共享内存。

**载体**（carrier）是承载虚拟线程执行的操作系统线程。库默认单载体；嵌入宿主经`VmExecutor::SetCarrierThreadCount`显式选入，`nvm`默认至多4个（`--carriers=N`，1..64）。多载体的并行只来自一类窗口：按ABI（应用二进制接口，application binary interface）第5版声明了阻塞段的原生调用——执行期间虚拟机交出GIL，其余载体可以运行其他虚拟线程（典型收益是把输入输出等待与其他工作重叠）；返回前重新取回GIL。未声明阻塞的第三方原生在整个调用期间持有GIL，其余载体全部停摆。

确定性表述：同一程序在相同输入下产生相同的交错顺序（FIFO队列＋固定回边配额）；改变载体数不改变不涉及阻塞原生的程序的可见行为——测试只断言顺序与终值，不断言时长。`sleep`只是带下界的调度提示：全部载体停泊后，睡眠者在操作系统定时器粒度内唤醒（Windows上约15.6ms）。调试器暂停时持GIL，全部虚拟线程冻结（见[已知限制](known-limitations.md)）。

## thread包

`thread`是标准库包：权威声明在`stdlib/thread.n`（符号索引与补全因此对它一视同仁），实现内建于虚拟机本体。调用前需`import thread;`（本页示例均已导入）；自建同名包会与标准库包撞名，被重复包规则拒绝。`spawn`的类型化契约——`func<T>`委托返回`Future<T>`——由编译器在声明的`func<void>`基础形态之上附加绑定。

| 成员 | 形态 | 语义 |
|---|---|---|
| `thread.spawn(f)` | `func<void>`零参委托 → `Thread`；`func<T>`零参委托 → `Future<T>` | 派生新虚拟线程，立即返回其句柄对象；返回类型决定句柄形态 |
| `thread.join(t)` | `Thread` | 挂起等待目标完成；目标失败时向本上下文重抛原异常对象 |
| `thread.yield()` | — | 主动让出，回到运行队列尾 |
| `thread.id()` | → `int` | 当前上下文的标识符；main恒为0，派生上下文非0 |
| `thread.sleep(ms)` | `int`毫秒 | 挂起到截止时间；`sleep(0)`等价于让出；负时长是编译期之后的运行期错误 |

### thread.spawn

参数必须是零参委托——自由函数引用、绑定方法引用或持有委托的变量皆可。委托无返回值（`func<void>`）时得到`Thread`句柄；有返回值（`func<T>`）时得到`Future<T>`句柄，返回值经`get()`取回（见下节）：

```nlang
import io;
import thread;

class Worker {
    public int tag;
    public void run() {
        io.write(tag.toString());
    }
}

int main() {
    Worker w = Worker();
    w.tag = 7;
    Thread t = thread.spawn(w.run);
    thread.join(t);
    return 0;
}
```

派生上下文与调用者共享同一堆：字段、容器与通道两边都直接可见。编译期拒绝非委托实参与带参数委托；指向`native`或内建函数的委托在运行期被拒绝。

## Future

`spawn`一个有返回值的委托（`func<T>`）得到`Future<T>`——返回值的取回句柄：

| 成员 | 语义 |
|---|---|
| `get()` | 阻塞取值：目标未完成则停泊，完成后按声明类型`T`交付返回值；目标失败时向本上下文重抛原异常对象（`join`同款） |
| `isDone()` | 非停泊探测：目标已完成（正常或失败）返回`true`，否则`false` |

```nlang
import io;
import thread;

int produce() {
    thread.sleep(10);
    return 42;
}

int main() {
    Future<int> f = thread.spawn(produce);
    io.write(f.get().toString());
    return 0;
}
```

`get()`是诚实的阻塞式等待（并发模型一节"无异步着色"的落点）：调用者停泊，值就绪后原地续行——这就是NLang的`await`。多个上下文可以`get()`同一个`Future`，全部都会被唤醒并拿到同一个值；`get()`已完成的目标准确返回，可重复调用。失败上下文的结果值不可取——`get()`重抛原异常对象（类型、`message`与`backtrace`字段原样）。`get()`自己派生的`Future`（把`Future`交回其派生者）是运行期错误，消息点名规则；此时没有任何事件能唤醒它，虚拟机会以死锁报告拦下（见[主退出策略与死锁报告](#主退出策略与死锁报告)）。

## thread.join

`join`挂起当前上下文直到目标完成。多个上下文可以`join`同一个目标，全部都会被唤醒。目标**失败**（未捕获异常退出）时，`join`向每个等待者重抛**原异常对象**——类型、`message`与`backtrace`字段都保持原样，因此`catch`到的就是派生上下文里抛出的那个实例：

```nlang
import io;
import thread;

void worker() {
    throw Exception("wboom");
}

int main() {
    Thread t = thread.spawn(worker);
    try {
        thread.join(t);
        return 4;
    } catch (Exception e) {
        if (e.message == "wboom") {
            return 0;
        }
        return 2;
    }
}
```

`join`已完成的目标准确返回，不重复等待；`join`自身（把`Thread`交回其派生者）是运行期错误。失败上下文保留的异常对象由调度器持有到运行结束（上下文记录不剪除），供任何迟到的`join`重抛。

## Mutex

`Mutex`是不可重入互斥体：持有期间再次`lock`会永久停泊（见下文死锁报告）。

| 成员 | 语义 |
|---|---|
| `lock()` | 获取互斥体；被持有时挂起排队，按到达顺序（先进先出）唤醒 |
| `unlock()` | 释放；未持有而`unlock`是运行期错误，消息点名规则 |
| `tryLock()` | 非停泊探测：成功返回`true`并持有，被持有返回`false` |

临界区内的`yield`与`sleep`照常让出——互斥保证不因让出而失效：

```nlang
import io;
import thread;

class Counter {
    public Mutex gate;
    public int n;
    public void bump() {
        gate.lock();
        n = n + 1;
        thread.yield();
        gate.unlock();
    }
}

int main() {
    Counter c = Counter();
    c.gate = Mutex();
    Thread a = thread.spawn(c.bump);
    Thread b = thread.spawn(c.bump);
    Thread d = thread.spawn(c.bump);
    thread.join(a);
    thread.join(b);
    thread.join(d);
    io.write(c.n.toString());
    return 0;
}
```

三个`bump`各让出一次，没有互斥时必然交错丢失更新；有互斥时输出恒为`3`。

## Channel

`Channel<T>`是消息通道，元素类型参数不可省略（裸`Channel`是编译错误——元素类型决定值的装箱与接收方拆箱）。两种构造形态：

```nlang
Channel<int> a = {};        //无界：缓冲没有上限，send永不挂起
Channel<int> b = Channel(2); //有界：容量2，缓冲满时send停泊
```

容量构造恰好取一个`int`：`0`等价无界，`n≥1`为有界容量，负值是运行期错误（消息给出取值范围）；空初始化器`{}`是无界缺省。有界通道的`send`在缓冲满时停泊排队（先进先出），`recv`取走一个值即腾出空位并唤醒队首发送者——先发的值先进缓冲，顺序保持。

| 成员 | 语义 |
|---|---|
| `send(v)` | 无界通道永不挂起；有界通道缓冲满时停泊排队，腾位后按到达顺序续行 |
| `recv()` | 缓冲非空时取队首返回；空则挂起直到`send`或`close` |
| `close()` | 关闭通道；幂等，缓冲区里已有的值不丢失 |

关闭语义以可捕获的`ChannelClosedException`为界：`close`后缓冲里已有的值照常被`recv`取走（已发送的值先于关闭信号送达）；缓冲排空后再`recv`、以及关闭后的`send`，都抛`ChannelClosedException`（`Exception`的子类，可`catch`恢复）。停泊中的等待者同样看到这条边界——`close`先排干缓冲交付，耗尽后剩余接收者与全部停泊发送者都以该异常唤醒。把"关闭"用作广播信号时，接收方用`try`/`catch`收尾：

```nlang
import io;
import thread;

class Reader {
    public Channel<int> ch;
    public int total;
    public void drain() {
        bool open = true;
        while (open) {
            try {
                total = total + ch.recv();
            } catch (ChannelClosedException e) {
                open = false;
            }
        }
    }
}

int main() {
    Reader r = Reader();
    r.ch = Channel(4);
    Thread t = thread.spawn(r.drain);
    r.ch.send(10);
    r.ch.send(20);
    r.ch.close();
    thread.join(t);
    io.write(r.total.toString());
    return 0;
}
```

基本类型元素发送时装箱、接收时拆箱（与`List<T>`同一机制）；引用元素直传。值`0`是合法装箱值，与「无值」语义天然可区分——关闭的边界由异常表达，不占用值域。

发送者先于接收者到达时，`recv`直接取缓冲；接收者先到则停泊，`send`追加并唤醒：

```nlang
import io;
import thread;

class Peer {
    public Channel<string> ch;
    public void shout() {
        ch.send("a");
        io.write("w");
    }
}

int main() {
    Peer p = Peer();
    p.ch = {};
    Thread t = thread.spawn(p.shout);
    io.write(p.ch.recv());
    thread.join(t);
    return 0;
}
```

main先停在`recv`；worker的`send`交付值并唤醒main，但worker自己不停泊，紧接着写出`w`，随后main恢复写出收到的`a`——输出`wa`，值已经从发送方流到了接收方。

## 标准库同步工具

内核原语之上，`concurrent`标准库包提供两个常用同步工具（`import concurrent;`后可用）：

| 类 | 形态 | 语义 |
|---|---|---|
| `WaitGroup` | `add(n)`／`done()`／`wait()` | 计数屏障（Go同形）：`add`累计待完成数，每个任务`done`一次，`wait`停泊到计数归零。契约：`add`先于`wait`，`done`与`add`配对——多出的`done`提前放行`wait`，缺失的`done`会停泊到死锁报告 |
| `Semaphore` | `Semaphore(n)`／`acquire()`／`release()` | 计数信号量：`acquire`取走一个许可（计数为0时让出等待），`release`归还一个 |

```nlang
import concurrent;
import io;
import thread;

class Job {
    public WaitGroup wg;
    public Mutex gate;
    public int total;
    public void work() {
        gate.lock();
        total = total + 10;
        gate.unlock();
        wg.done();
    }
}

int main() {
    Job j = Job();
    j.gate = Mutex();
    j.wg = WaitGroup();
    j.wg.add(4);
    Thread a = thread.spawn(j.work);
    Thread b = thread.spawn(j.work);
    Thread c = thread.spawn(j.work);
    Thread d = thread.spawn(j.work);
    j.wg.wait();
    io.write(j.total.toString());
    thread.join(a);
    thread.join(b);
    thread.join(c);
    thread.join(d);
    return 0;
}
```

`wait`是真实的屏障——读到`40`当且仅当四次累加全部落在读之前。`Semaphore`当前以让出等待实现（内核尚无条件变量原语），许可为0时的`acquire`靠`thread.yield`轮转让位，不占载体。

## 异常与停泊的正交性

停泊（`sleep`、`join`、竞争`lock`、空`recv`、被抢占）完整保留上下文的执行状态：帧、局部变量、`try`/`catch`/`finally`的处理器状态与指令位置全部原样冻结，恢复后续跑。因此：

- `try`体内停泊，恢复后`throw`照常落入该`try`的`catch`（跨帧同样成立）；
- `finally`在停泊过的`try`体完成后照常执行且恰执行一次；
- 已进入的`catch`处理器体内停泊，恢复后`throw;`重抛原异常照常工作。

派生上下文的**未捕获**异常不会传给宿主或main：该上下文标记失败、帧丢弃，失败只经`join`以原异常对象浮出。

## 主退出策略与死锁报告

`main`返回即程序结束：仍停泊或排队的上下文被静默丢弃（没有隐式的等待全部——一个沉睡的worker不该挂死整个程序；见[已知限制](known-limitations.md)）。

当`main`停泊且再无可运行上下文（例如空通道`recv`永远等不到发送者、互斥体自重锁），虚拟机以运行期错误报告死锁——消息含`deadlock`——而不是挂死进程；工具与嵌入宿主都可捕获该错误。

## 嵌套求值窗口内的限制

`toString`求值等嵌套窗口内不能挂起：`thread.yield`、`thread.sleep`、未完成目标的`thread.join`、未完成目标的`Future.get`、竞争中的`lock`、空通道`recv`、缓冲已满的有界通道`send`一律以运行期错误拒绝（消息含`cannot suspend`），`thread.spawn`同样拒绝。嵌入宿主的`call()`窗口属同一形态，见[嵌入NLang](../libraries/embedding-api.md)。
