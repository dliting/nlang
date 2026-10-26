# Semaphore

## 何时使用

限制同时进入某段代码的任务数——资源池、连接数上限、最多N份并发计算——就用`Semaphore(n)`：进入前`acquire()`取一个许可（permit），离开时`release()`归还，一进一出配对，计数就不会失真。与`Mutex`的差别：互斥体是「同一时间恰好一个」，信号量是「同一时间至多N个」。一条经验划清三种路线：共享的状态用[互斥体](concurrency-mutex.md)，传递的数据用[通道](concurrency-channel.md)，限制并发数用信号量。

## 许可

`Semaphore`声明于预置包`builtins`（`stdlib/builtins.n`），每次构建自动导入，无需import语句。构造`Semaphore(n)`设定初始许可数`n`：负数是运行期错误，`0`合法——「信号开关」形态，先造一个零许可信号量，等将来某个事件`release`一声，等待者才放行。

| 成员 | 语义 |
|---|---|
| `acquire()` | 取走一个许可；许可为零时挂起排队，按到达顺序（先进先出）唤醒——与`lock`同款真实挂起，不是轮询 |
| `release()` | 归还一个许可：有等待者时直接移交给队首（计数不变），否则计数加一；没有持有前提，未`acquire`也可`release`，计数可以超过初值（与Java、Python一致） |
| `tryAcquire()` | 非挂起探测：许可可得返回`true`并取走，为零返回`false` |

超释放不是错误，但会让计数虚高、限流失真——`acquire`与`release`的配对纪律同`lock`/`unlock`。

限流探针——三个任务抢两个许可，重叠峰值恒为`2`（安装包的`examples/conc_semaphore.n`是它的四任务变体）：

```nlang
import io;
import thread;

class Worker {
    public Semaphore slots;
    public Mutex gate;
    public int inside;
    public int peak;
    public void work() {
        slots.acquire();
        gate.lock();
        inside = inside + 1;
        if (inside > peak) {
            peak = inside;
        }
        gate.unlock();
        thread.yield();
        gate.lock();
        inside = inside - 1;
        gate.unlock();
        slots.release();
    }
}

int main() {
    Worker w = Worker();
    w.slots = Semaphore(2);
    w.gate = Mutex();
    Thread a = thread.spawn(w.work);
    Thread b = thread.spawn(w.work);
    Thread c = thread.spawn(w.work);
    thread.join(a);
    thread.join(b);
    thread.join(c);
    io.write(w.peak.toString());
    return 0;
}
```

## 信号开关与轮询

零许可信号量当开关用：等待者停在`acquire()`上，事件一侧`release()`放行：

```nlang
import io;
import thread;

class Starter {
    public Semaphore gate;
    public void run() {
        gate.acquire();
        io.write("started");
    }
}

int main() {
    Starter s = Starter();
    s.gate = Semaphore(0);
    Thread t = thread.spawn(s.run);
    thread.sleep(5);
    io.write("go");
    s.gate.release();
    thread.join(t);
    return 0;
}
```

输出恒为`gostarted`——`run`在`release`之前不可能越过`acquire`。

`tryAcquire()`配`sleep`组出「现在不行就先歇会儿再试」的轮询形态——`sleep`以毫秒计，不是自旋：许可没到，任务就在`sleep`里歇着，到点才醒来重试一次：

```nlang
import io;
import thread;

class Taster {
    public Semaphore pool;
    public void fill() {
        thread.sleep(10);
        pool.release();
    }
    public void poll() {
        while (!pool.tryAcquire()) {
            thread.sleep(2);
        }
        io.write("granted");
    }
}

int main() {
    Taster t = Taster();
    t.pool = Semaphore(0);
    Thread f = thread.spawn(t.fill);
    Thread p = thread.spawn(t.poll);
    thread.join(f);
    thread.join(p);
    return 0;
}
```

`tryAcquire()`绝不挂起，因此重入求值（如`toString`覆写）内也可安全调用；零许可`acquire`则与竞争中的`lock`一样被拒（见[重入求值内的限制](concurrency-suspension.md#重入求值内的限制)）。

## 与Channel对照

有界通道也能表达「至多N个」：容量N的通道满了，`send`自然挂起。但通道传递的是**数据**——每个`send`都携带一个值；信号量表达的是**计数准入**——`release`不携带任何东西。需要把「进入许可」和「取到的数据」绑在一起（工作队列），用通道；只需要数着名额放人（连接池、并发上限），用信号量。
