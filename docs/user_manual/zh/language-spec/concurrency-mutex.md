# Mutex

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
