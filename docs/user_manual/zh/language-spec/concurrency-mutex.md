# Mutex

## 何时使用

多个任务读写同一份可变状态——计数器、缓存、标志位——就用`Mutex`：锁让每次"读-改-写"成为一个原子序列，更新不会丢。`tryLock()`适合"试一下、不等待"的形态：锁空闲就拿到，被占用就走别的路（本轮跳过、改用备用方案），而不是挂起排队。

一条经验划清两种同步路线：共享的状态用互斥体，传递的数据用[通道](concurrency-channel.md)。

## 锁

`Mutex`是不可重入互斥体：持有期间再次`lock`会永久停泊（见[死锁报告](concurrency-parking.md#主退出策略与死锁报告)）。

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
