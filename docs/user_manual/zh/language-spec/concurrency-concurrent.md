# 标准库同步工具

规范性规则见规范§11.3。
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
