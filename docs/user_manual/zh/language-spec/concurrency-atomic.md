# AtomicInt

## 何时使用

`AtomicInt`承载跨载体共享的**单值**可变状态——计数器、标志位、序列号。字段读写本身不加锁（见[载体与并行](concurrency-carriers.md)），共享字段的普通读写可能读到过渡值；`AtomicInt`把「读-改-写」变成不可分割的单步，无需互斥体。

选择指引：**单个值的原子更新**用`AtomicInt`；**跨多个字段的不变式**用[Mutex](concurrency-mutex.md)（原子只保证单值独立一致，`x = x + 1`与`y = y + 2`的组合一致仍需互斥体）；方法组合或递归加锁用[ReentrantMutex](concurrency-reentrant-mutex.md)。

## 操作

命名沿用Java`AtomicInteger`的规约：`getAndX`返回**旧值**，`xAndGet`返回**新值**。

| 成员 | 语义 |
|---|---|
| `AtomicInt()` | 初值0 |
| `AtomicInt(int initial)` | 初值`initial` |
| `load()` | 原子读，返回当前值 |
| `store(int v)` | 原子写 |
| `getAndSet(int v)` | 原子置`v`，返回旧值 |
| `compareExchange(int expect, int v)` | 比较交换（CAS）：当前值等于`expect`时置`v`并返回`true`；否则值不变、返回`false` |
| `getAndAdd(int delta)` | 原子加，返回加前值 |
| `addAndGet(int delta)` | 原子加，返回加后值 |
| `getAndIncrement()` / `incrementAndGet()` | +1的两面（返旧/返新） |

所有操作**都不挂起**——重入求值内（`toString`覆写、宿主`call()`）照常可用，与`tryLock()`同权。溢出按int32环绕（与平台加法一致）。

## 计数器

```nlang
import thread;
import io;
import concurrent;

class Bumper {
    public AtomicInt counter;

    public void bump() {
        int i = 0;
        while (i < 2500) {
            counter.addAndGet(1);   //读-改-写一步完成，无丢更新
            i = i + 1;
        }
    }
}

int main() {
    Bumper b = Bumper();
    b.counter = AtomicInt(0);
    Thread a = thread.spawn(b.bump);
    Thread c = thread.spawn(b.bump);
    thread.join(a);
    thread.join(c);
    io.print(b.counter.load());   //恒为5000
    return 0;
}
```

四个任务四载体并发自增，终值精确落点——`counter.n = counter.n + 1`这种字段写法在同样交错下会丢更新。

## 标志位（只许一次）

`compareExchange`是「读-判-写」唯一的原子复合，天然表达只许一次的初始化：

```nlang
if (flag.compareExchange(0, 1)) {
    //只有第一个把0换成1的任务进这里
    init();
}
```

## 与Mutex的分工

| 形态 | 用 |
|---|---|
| 单值计数器/标志位/序列号 | `AtomicInt` |
| 跨多字段的不变式 | [Mutex](concurrency-mutex.md) |
| 方法组合或递归加锁 | [ReentrantMutex](concurrency-reentrant-mutex.md) |
| 限制并发进入数 | [Semaphore](concurrency-semaphore.md) |

`AtomicInt`可与互斥体并存：热点计数走原子，多字段状态走互斥体，各护其位。
