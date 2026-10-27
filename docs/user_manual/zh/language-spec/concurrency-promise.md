# Promise

## 何时使用

`Future`的值来自派生出去的函数返回——`thread.spawn`是唯一的取回句柄来源。可有时候值并不来自任何派生出去的函数：宿主环境的回调要把外部事件送进等待世界；一个事件要同时唤醒多处等待；或者测试需要精确控制「值何时就绪」。`Promise<T>`是手工完成源：构造一个待完成的`Promise`，把它的`Future`交出去，自己在任意上下文、任意时刻填值——恰好一次。

```nlang
import io;
import thread;

class Waiter {
    public Promise<int> p;
    public void fire() { p.complete(42); }
}

int main() {
    Promise<int> p = Promise();
    Future<int> f = p.future();
    Waiter w = Waiter();
    w.p = p;
    Thread t = thread.spawn(w.fire);
    io.write(f.get().toString());
    thread.join(t);
    return 0;
}
```

## 成员

| 成员 | 语义 |
|---|---|
| `Promise()` | 构造待完成源：立即得到一个可分发的未决定`Future`，不需要先有函数 |
| `future()` | 分发取回句柄。可重复调用：每个`Future`独立`get()`，共享同一个决定 |
| `complete(v)` | 以值`v`完成：第一个调用获胜，之后的调用静默无效；所有等待者被唤醒并拿到`v` |
| `completeExceptionally(e)` | 以异常对象`e`失败完成：同样先完成者获胜；等待中的`get()`向各自上下文重抛`e` |

先完成者获胜（first-wins）是全部语义的骨架：值与异常是同一种「完成」，一次决定，永不改写。完成可以来自任何上下文——主上下文直接完成、派生任务跨上下文完成、多个上下文竞争完成，都成立；并发`complete`由调度器排序，恰好一个生效。`Promise`本身是共享对象的普通字段语义（NLang没有闭包，跨上下文共享经类字段或容器传递）。

## Future.selectAny：多路等待

静态调用`Future.selectAny(futures)`接收一个`Future`数组，挂起调用者直到**任意一个**成员决定，返回获胜成员的下标（多个已决定时取最小下标）：

```nlang
import io;

int main() {
    Promise<int> pa = Promise();
    Promise<int> pb = Promise();
    Future<int> a = pa.future();
    Future<int> b = pb.future();
    Future<int>[] arr = [a, b];
    int winner = Future.selectAny(arr);
    io.write(winner.toString());
    io.write(" ");
    io.write(a.get().toString());
    io.write("\n");
    pb.complete(2);
    pa.complete(1);
    return 0;
}
```

- **失败也算决定**：失败的目标同样能让`selectAny`返回；但观察失败是`get()`的事——获胜下标不抛异常，对获胜者调`get()`才重抛它的原异常对象。
- **败者继续运行**：其余成员照常推进，其结果不受影响；之后随时可以`get()`。从未被观测的失败仍会在运行收尾时报告（无人观测的派生任务失败会被点名，见[挂起与调度限制](concurrency-suspension.md)）。
- **重复成员合法**：同一个`Future`出现在多个下标时，最小下标获胜。
- **边界**：空数组、含`null`成员、超过64个成员、把当前上下文自己的`Future`放进等待集，都是运行期错误；自等待与`get()`自等是同一个拒绝，消息点名规则。

在`selectAny`之前，「任意一个先就绪」要为每个源派生一个等待者经通道汇合——能写，但每路一个派生加一次通道往返。`selectAny`把同一形态压成一行静态调用；对照示例见安装包`examples/conc_*.n`。

## 已知边界

- **永不完成是静默的**：`Promise`没有超时；一个永不完成的源会让等待者一直挂起，最终由死锁报告拦下（见[挂起与调度限制](concurrency-suspension.md#主退出策略与死锁报告)）。需要限时就用「等待者加定时器」的组合：等待与`thread.sleep`分别派生，先醒者决定走哪条路。
- **多路等待限于`Future`**：通道上没有「任意一个就绪」的`recv`变体。多源通道要任意收，为每个源经`Promise`组合一层（每个源一个转发任务，收到的值`complete`给各自的`Promise`），再对`Future`数组`selectAny`。
- **带返回值的`spawn`接收裸函数引用或已绑定的委托值**：`thread.spawn(obj.method)`直接派生带返回值的委托当前要求先经`func<int> d = obj.method;`这样的显式委托局部（或改用自由函数）；无返回值形态不受影响。
