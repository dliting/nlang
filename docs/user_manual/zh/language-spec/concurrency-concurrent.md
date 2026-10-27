# 标准库同步工具

## 何时使用

`WaitGroup`等一批派生任务全部完成：派生前先`add`好单位数，每个任务收尾时`done`一次，`wait`在整批完成时返回——不需要持有任何`Thread`句柄。限制并发数的`Semaphore`是内建句柄类，见[Semaphore](concurrency-semaphore.md)。

## 工具

内核原语之上，`concurrent`标准库包提供`WaitGroup`（`import concurrent;`后可用）：计数屏障（Go同形）——`add(n)`累计待完成数，每个任务`done()`一次，`wait()`挂起到计数归零。契约：`add`先于`wait`，`done`与`add`配对——多出的`done`提前放行`wait`，缺失的`done`会挂起到死锁报告。

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
