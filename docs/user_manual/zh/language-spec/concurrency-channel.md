# Channel

规范性规则见规范§11.4。
`Channel<T>`是消息通道，元素类型参数不可省略（裸`Channel`是编译错误——元素类型决定值的装箱与接收方拆箱）。两种构造形态：

```nlang
Channel<int> a = {};        //无界：缓冲没有上限，send永不挂起
Channel<int> b = Channel(2); //有界：容量2，缓冲满时send停泊
```

容量构造恰好取一个`int`：`0`等价无界，`n≥1`为有界容量，负值是运行期错误（消息给出取值范围）；空初始化器`{}`是无界缺省。有界通道的`send`在缓冲满时停泊排队（先进先出），`recv`取走一个值即腾出空位并唤醒队首发送者——先发的值先进缓冲，顺序保持。

| 成员 | 语义 |
|---|---|
| `send(v)` | 无界通道永不挂起；有界通道缓冲满时停泊排队，腾位后按到达顺序续行 |
| `recv()` | 缓冲非空时取队首返回；空则挂起直到`send`或`close` |
| `close()` | 关闭通道；幂等，缓冲区里已有的值不丢失 |

关闭语义以可捕获的`ChannelClosedException`为界：`close`后缓冲里已有的值照常被`recv`取走（已发送的值先于关闭信号送达）；缓冲排空后再`recv`、以及关闭后的`send`，都抛`ChannelClosedException`（`Exception`的子类，可`catch`恢复）。停泊中的等待者同样看到这条边界——`close`先排干缓冲交付，耗尽后剩余接收者与全部停泊发送者都以该异常唤醒。把"关闭"用作广播信号时，接收方用`try`/`catch`收尾：

```nlang
import io;
import thread;

class Reader {
    public Channel<int> ch;
    public int total;
    public void drain() {
        bool open = true;
        while (open) {
            try {
                total = total + ch.recv();
            } catch (ChannelClosedException e) {
                open = false;
            }
        }
    }
}

int main() {
    Reader r = Reader();
    r.ch = Channel(4);
    Thread t = thread.spawn(r.drain);
    r.ch.send(10);
    r.ch.send(20);
    r.ch.close();
    thread.join(t);
    io.write(r.total.toString());
    return 0;
}
```

基本类型元素发送时装箱、接收时拆箱（与`List<T>`同一机制）；引用元素直传。值`0`是合法装箱值，与「无值」语义天然可区分——关闭的边界由异常表达，不占用值域。

发送者先于接收者到达时，`recv`直接取缓冲；接收者先到则停泊，`send`追加并唤醒：

```nlang
import io;
import thread;

class Peer {
    public Channel<string> ch;
    public void shout() {
        ch.send("a");
        io.write("w");
    }
}

int main() {
    Peer p = Peer();
    p.ch = {};
    Thread t = thread.spawn(p.shout);
    io.write(p.ch.recv());
    thread.join(t);
    return 0;
}
```

main先停在`recv`；worker的`send`交付值并唤醒main，但worker自己不停泊，紧接着写出`w`，随后main恢复写出收到的`a`——输出`wa`，值已经从发送方流到了接收方。
