# 停泊与调度限制

## 异常与停泊的正交性

停泊（`sleep`、`join`、竞争`lock`、空`recv`、被抢占）完整保留上下文的执行状态：帧、局部变量、`try`/`catch`/`finally`的处理器状态与指令位置全部原样冻结，恢复后续跑。因此：

- `try`体内停泊，恢复后`throw`照常落入该`try`的`catch`（跨帧同样成立）；
- `finally`在停泊过的`try`体完成后照常执行且恰执行一次；
- 已进入的`catch`处理器体内停泊，恢复后`throw;`重抛原异常照常工作。

派生上下文的**未捕获**异常不会传给宿主或main：该上下文标记失败、帧丢弃，失败只经`join`（或对该上下文的`Future`调用`get()`）以原异常对象浮出。

## 主退出策略与死锁报告

`main`返回即程序结束：仍停泊或排队的上下文被静默丢弃（没有隐式的等待全部——一个沉睡的worker不该挂死整个程序；见[已知限制](known-limitations.md)）。

当`main`停泊且再无可运行上下文（例如空通道`recv`永远等不到发送者、互斥体自重锁），虚拟机以运行期错误报告死锁——消息含`deadlock`——而不是挂死进程；工具与嵌入宿主都可捕获该错误。

## 嵌套求值窗口内的限制

`toString`求值等嵌套窗口内不能挂起：`thread.yield`、`thread.sleep`、未完成目标的`thread.join`、未完成目标的`Future.get`、竞争中的`lock`、空通道`recv`、缓冲已满的有界通道`send`一律以运行期错误拒绝（消息含`cannot suspend`），`thread.spawn`同样拒绝。嵌入宿主的`call()`窗口属同一形态，见[嵌入NLang](../libraries/embedding-api.md)。
