# thread包

`thread`是标准库包：权威声明在`stdlib/thread.n`（符号索引与补全因此对它一视同仁），实现内建于虚拟机本体。调用前需`import thread;`（本章示例均已导入）；自建同名包会与标准库包撞名，被重复包规则拒绝。`spawn`的类型化契约——`func<T>`委托返回`Future<T>`——由编译器在声明的`func<void>`基础形态之上附加绑定。

| 成员 | 形态 | 语义 |
|---|---|---|
| `thread.spawn(f)` | `func<void>`零参委托 → `Thread`；`func<T>`零参委托 → `Future<T>` | 派生新虚拟线程，立即返回其句柄对象；返回类型决定句柄形态 |
| `thread.join(t)` | `Thread` | 挂起等待目标完成；目标失败时向本上下文重抛原异常对象 |
| `thread.yield()` | — | 主动让出，回到运行队列尾 |
| `thread.id()` | → `int` | 当前上下文的标识符；main恒为0，派生上下文非0 |
| `thread.sleep(ms)` | `int`毫秒 | 挂起到截止时间；`sleep(0)`等价于让出；负时长是编译期之后的运行期错误 |

## thread.spawn

参数必须是零参委托——自由函数引用、绑定方法引用或持有委托的变量皆可。委托无返回值（`func<void>`）时得到`Thread`句柄；有返回值（`func<T>`）时得到`Future<T>`句柄，返回值经`get()`取回（见[Future](concurrency-future.md)）：

```nlang
import io;
import thread;

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

派生上下文与调用者共享同一堆：字段、容器与通道两边都直接可见。编译期拒绝非委托实参与带参数委托；指向`native`或内建函数的委托在运行期被拒绝。

## thread.join

`join`挂起当前上下文直到目标完成。多个上下文可以`join`同一个目标，全部都会被唤醒。目标**失败**（未捕获异常退出）时，`join`向每个等待者重抛**原异常对象**——类型、`message`与`backtrace`字段都保持原样，因此`catch`到的就是派生上下文里抛出的那个实例：

```nlang
import io;
import thread;

void worker() {
    throw Exception("wboom");
}

int main() {
    Thread t = thread.spawn(worker);
    try {
        thread.join(t);
        return 1;              // join 会重抛失败的上下文：此路不可达
    } catch (Exception e) {
        if (e.message == "wboom") {
            return 30;         // 正是 worker 里抛出的那个实例
        }
        return 1;
    }
}
```

`join`已完成的目标准确返回，不重复等待；`join`自身（把`Thread`交回其派生者）是运行期错误。失败上下文保留的异常对象由调度器持有到运行结束（上下文记录不剪除），供任何迟到的`join`重抛。
