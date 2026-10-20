# 并发

NLang提供轻量并发：`thread`命名空间派生**虚拟线程**——共享载体池、在全局解释器锁下每次只运行一个的执行上下文，不是各自独立的操作系统线程。`Mutex`与`Channel<T>`提供互斥与传递式同步；`join`建立完成顺序。全部原语无需`import`即可使用（`thread`是保留包名，自建同名包会被编译器拒绝并提示改名）。

调度是完全确定性的：载体池按先进先出（FIFO，first in first out）队列轮转上下文，每上下文跑满固定回边配额才让位（仅当另有可运行上下文时）；`sleep`按截止时间排序唤醒。同一程序在同样的输入下产生同样的交错顺序——测试只断言顺序与终值，不断言时长。

## GIL与多载体

虚拟机持有**全局解释器锁**（GIL，global interpreter lock）：任意时刻恰有一个虚拟线程在执行字节码，上下文切换只发生在安全点（函数入口与循环回边），并受每约1000个回边一次的调度预算约束。字节码层的互斥由GIL保证，语言代码无需担心两个虚拟线程同时改写共享内存。

**载体**（carrier）是承载虚拟线程执行的操作系统线程。库默认单载体；嵌入宿主经`VmExecutor::SetCarrierThreadCount`显式选入，`nvm`默认至多4个（`--carriers=N`，1..64）。多载体的并行只来自一类窗口：按ABI（应用二进制接口，application binary interface）第5版声明了阻塞段的原生调用——执行期间虚拟机交出GIL，其余载体可以运行其他虚拟线程（典型收益是把输入输出等待与其他工作重叠）；返回前重新取回GIL。未声明阻塞的第三方原生在整个调用期间持有GIL，其余载体全部停摆。

确定性表述：同一程序在相同输入下产生相同的交错顺序（FIFO队列＋固定回边配额）；改变载体数不改变不涉及阻塞原生的程序的可见行为——测试只断言顺序与终值，不断言时长。`sleep`只是带下界的调度提示：全部载体停泊后，睡眠者在操作系统定时器粒度内唤醒（Windows上约15.6ms）。调试器暂停时持GIL，全部虚拟线程冻结（见[已知限制](known-limitations.md)）。

## thread命名空间

| 成员 | 形态 | 语义 |
|---|---|---|
| `thread.spawn(f)` | `func<void>`零参委托 → `Thread` | 派生新虚拟线程，立即返回其`Thread`对象 |
| `thread.join(t)` | `Thread` | 挂起等待目标完成；目标失败时向本上下文重抛原异常对象 |
| `thread.yield()` | — | 主动让出，回到运行队列尾 |
| `thread.id()` | → `int` | 当前上下文的标识符；main恒为0，派生上下文非0 |
| `thread.sleep(ms)` | `int`毫秒 | 挂起到截止时间；`sleep(0)`等价于让出；负时长是编译期之后的运行期错误 |

### thread.spawn

参数必须是零参的`func<void>`委托——自由函数引用、绑定方法引用或持有委托的变量皆可：

```nlang
import io;

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

派生上下文与调用者共享同一堆：字段、容器与通道两边都直接可见。编译期拒绝非委托实参、带参数委托与带返回值委托（`func<void>`是唯一形态）；指向`native`或内建函数的委托在运行期被拒绝。

### thread.join

`join`挂起当前上下文直到目标完成。多个上下文可以`join`同一个目标，全部都会被唤醒。目标**失败**（未捕获异常退出）时，`join`向每个等待者重抛**原异常对象**——类型、`message`与`backtrace`字段都保持原样，因此`catch`到的就是派生上下文里抛出的那个实例：

```nlang
import io;

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

`Channel<T>`是无界消息通道，元素类型参数不可省略（裸`Channel`是编译错误——元素类型决定值的装箱与接收方拆箱）。唯一构造形态是空初始化器：

```nlang
Channel<int> ci = {};
Channel<string> cs = {};
```

| 成员 | 语义 |
|---|---|
| `send(v)` | 追加到缓冲区，**永不挂起**；通道已关闭时是运行期错误 |
| `recv()` | 缓冲非空时取队首返回；空则挂起直到`send`或`close` |
| `close()` | 关闭通道；幂等，缓冲区里已有的值不丢失 |

关闭语义：`recv`先排干缓冲值，缓冲耗尽且已关闭时返回`null`。元素类型是类、接口等引用类型时可直接`== null`判断；基本类型元素（如`Channel<int>`）经拆箱交付，`null`无法表示——对已关闭排空的整型通道`recv`表现为运行期错误，需要哨兵值语义请用引用元素类型。

基本类型元素发送时装箱、接收时拆箱（与`List<T>`同一机制）；引用元素直传。值`0`是合法装箱值，与「无值」的`null`哨兵天然可区分。

发送者先于接收者到达时，`recv`直接取缓冲；接收者先到则停泊，`send`追加并唤醒：

```nlang
import io;

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

`toString`求值等嵌套窗口内不能挂起：`thread.yield`、`thread.sleep`、未完成目标的`thread.join`、竞争中的`lock`、空通道`recv`一律以运行期错误拒绝（消息含`cannot suspend`），`thread.spawn`同样拒绝。嵌入宿主的`call()`窗口属同一形态，见[嵌入NLang](../libraries/embedding-api.md)。
