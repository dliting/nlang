# Future

`spawn`一个有返回值的委托（`func<T>`）得到`Future<T>`——返回值的取回句柄：

| 成员 | 语义 |
|---|---|
| `get()` | 阻塞取值：目标未完成则停泊，完成后按声明类型`T`交付返回值；目标失败时向本上下文重抛原异常对象（`join`同款） |
| `isDone()` | 非停泊探测：目标已完成（正常或失败）返回`true`，否则`false` |

```nlang
import io;
import thread;

int produce() {
    thread.sleep(10);
    return 42;
}

int main() {
    Future<int> f = thread.spawn(produce);
    io.write(f.get().toString());
    return 0;
}
```

`get()`是诚实的阻塞式等待（[并发模型](concurrency.md#并发模型阻塞式书写无异步着色)一节"无异步着色"的落点）：调用者停泊，值就绪后原地续行——这就是NLang的`await`。多个上下文可以`get()`同一个`Future`，全部都会被唤醒并拿到同一个值；`get()`已完成的目标立即返回，可重复调用。失败上下文的结果值不可取——`get()`重抛原异常对象（类型、`message`与`backtrace`字段原样）。派生上下文自身不能`get()`自己的`Future`：调用立即抛出点名规则的运行时错误（`a future cannot be awaited by its own context`）——没有任何事件能唤醒该上下文。派生者`get()`自己派生的`Future`是正常用法（见上方示例）。
