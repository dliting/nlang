# ReentrantMutex

## 何时使用

`ReentrantMutex`是`Mutex`的可重入对位：同一个上下文持有期间可以再次`lock`而不挂起。它解决`Mutex`最容易踩的坑——**方法组合**。一个公共方法持锁后调用同类的另一个加锁方法，在`Mutex`上就是自死锁（挂起直到死锁报告）；方法组合是最普通的写法，递归同理。

选择指引：默认用`Mutex`——不可重入让「加了锁却没释放」的设计错误能被死锁报告暴露；当锁的方法会组合或递归（外层与内层都加同一把锁）时用`ReentrantMutex`。一把`ReentrantMutex`上「忘掉释放」依然会被死锁报告抓住——可重入只豁免同上下文的重复获取，不豁免永不释放。

## 锁

| 成员 | 语义 |
|---|---|
| `lock()` | 空闲则取得；**自持有则持有计数+1、立即返回**（不停泊）；他持有则挂起排队，按到达顺序（先进先出）唤醒 |
| `unlock()` | 持有计数-1，**归零才**释放或移交；未持有而`unlock`是运行期错误，消息点名规则 |
| `tryLock()` | 非挂起探测：空闲或自持有返回`true`（自持有同时计数+1），他持有返回`false` |

移交语义与`Mutex`一致：归零时把所有权交给队首等待者，新持有者从计数1重新开始——持有计数不跨上下文。竞争`lock()`的停泊走与`Mutex`相同的握手，重入求值内照常挂起（唯一仍拒绝的形态是无载体的宿主`call()`窗口，`cannot suspend`）；`tryLock()`从不停泊。

## 组合方法

```nlang
import io;
import thread;

class Service {
    public ReentrantMutex gate;
    public int count;

    public void outer() {
        gate.lock();
        this.inner();      //inner再加同一把锁——计数+1，不挂起
        count = count + 1;
        gate.unlock();
    }

    public void inner() {
        gate.lock();
        count = count + 10;
        gate.unlock();
    }
}

int main() {
    Service s = Service();
    s.gate = ReentrantMutex();
    Thread t = thread.spawn(s.outer);
    thread.join(t);
    io.print(s.count);     //11：两层lock/unlock平衡归零
    return 0;
}
```

`outer`持有后进入`inner`再`lock`——在`Mutex`上这里就是自死锁；`ReentrantMutex`上持有计数走到2，两次`unlock`归零，锁才对其他上下文可见。

## 递归

递归函数每层加锁、每层释放，与组合同源——深度8的递归在退出时持有计数恰好归零，锁的正确性不因深度改变。这正对应Python`threading.RLock`与Java`ReentrantLock`的持有计数模型。

## 与Mutex的分工

| 形态 | 用 |
|---|---|
| 护一段临界区，方法间不组合 | [Mutex](concurrency-mutex.md) |
| 公共方法与它调用的方法都加锁 | `ReentrantMutex` |
| 递归体内加锁 | `ReentrantMutex` |
| 计数限流 | [Semaphore](concurrency-semaphore.md) |

两者可以并存：各自的锁互不干扰，同一程序里按成员选用。
