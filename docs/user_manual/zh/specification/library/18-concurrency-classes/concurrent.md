> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 18.11 concurrent包

`concurrent`的各句柄类各有专页（§18.6–§18.10）；本页覆盖该包唯一的普通NLang类`WaitGroup`。包形态在§18.2，语义在§10.3。

## 何时使用

`WaitGroup`等一批派生任务全部完成：派生前先`add`好单位数，每个任务收尾时`done`一次，`wait`在整批完成时返回——不需要持有任何`Thread`句柄。限制并发数的`Semaphore`是同一个包的句柄类，见[Semaphore](semaphore.md)。

## 工具

一句`import concurrent;`把整个配合侧带入作用域：句柄类`Mutex`、`ReentrantMutex`、`Semaphore`、`AtomicInt`与`Channel<T>`，异常子类`ChannelClosedException`，以及`WaitGroup`——该包唯一的普通NLang类，计数屏障（Go同形）——`add(n)`累计待完成数，每个任务`done()`一次，`wait()`挂起到计数归零。契约：`add`先于`wait`，`done`与`add`配对——不配对的`done`会让`wait`提前放行，缺失的`done`会让它永远停泊——程序在§10.1的退出策略下结束。各句柄类各有专页（§18.6–§18.10；执行模型总览见[并发](../../10-concurrency/index.md)）。

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

`wait`是真实的屏障——读到`40`当且仅当四次累加全部落在读之前。
