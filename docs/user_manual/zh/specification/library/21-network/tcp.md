> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 21.2 传输控制协议（TCP）类

传输控制协议（TCP，Transmission Control Protocol）是面向连接、有序、可靠的字节流。`TcpListener`接受进来的连接；`TcpStream`是一条已建立的连接。停泊与超时约定、每方向一个操作的规则都在§21.1：每个阻塞方法停泊调用上下文，在截止时刻抛`TimeoutException`，在套接字已关闭或失败时抛`SocketException`。

## TcpListener

| 成员 | 形态 | 语义 |
|---|---|---|
| `TcpListener.bind(host, port, backlog = 512)` | 静态→`TcpListener` | 在`host:port`上绑定并监听；`backlog`是交给操作系统的接受队列深度（连接风暴前调大——队列满时新连接被拒绝；内核会压缩该值，且Windows环回无论请求多大只保留约200个未决连接，风暴中被拒的客户端应当重试）。地址被占用或不可用时抛`SocketException` |
| `TcpListener.bind(port)` | 静态→`TcpListener` | `bind("0.0.0.0", port)`的简写——全部接口 |
| `accept(timeoutMs = 0)` | →`TcpStream` | 等待下一个连接，把它作为自己的`TcpStream`返回；停泊直到有连接到来或截止时刻到来 |
| `localAddress()` | →`string` | 绑定的地址 |
| `localPort()` | →`int` | 绑定的端口——`bind`用端口`0`时即系统分配的端口 |
| `setRecvBufferSize(bytes)` | — | 为经本监听器接受的连接请求接收缓冲区大小；须在连接建立之前设置才生效 |
| `close()` | — | 停止监听；幂等。已接受的连接不受影响 |

`accept`的`TimeoutException`是轮询工具而非错误：还要照看关停标志的服务器循环捕获它再循环一轮——随包发布的`examples/net_echo_server.n`正是这么做的，关闭监听器则经`SocketException`终结循环。

## TcpStream

| 成员 | 形态 | 语义 |
|---|---|---|
| `TcpStream.connect(host, port, timeoutMs = 0)` | 静态→`TcpStream` | 解析`host`并建立连接，把解析出的每个地址依次试一遍；解析与等待都停泊调用方（§21.1）；截止时刻到来仍无地址接受连接时抛`TimeoutException`，解析失败或所有连接尝试被拒时抛`SocketException` |
| `read(buf, timeoutMs = 0)` | →`int` | 用非空`buf`最多填`buf.length`个字节并返回个数；`0`表示流结束——对端关闭了它的写端；`buf`为空抛`SocketException`，因为`0`已留给流结束这个信号。字节按序到达；一次`read`可能返回少于请求的字节数 |
| `write(data, count = -1, timeoutMs = 0)` | →`int` | 发送`data`的前`count`个字节——`count`为`-1`时全发；停泊直到字节移交给协议栈。`timeoutMs`约束整个写全量：对端停止读取且截止时刻到来时抛`TimeoutException`，此时载荷可能已经发出了一部分 |
| `write(s, timeoutMs = 0)` | →`int` | 文本简写：把`s`编码为Unicode转换格式（UTF-8）字节并发送，超时行为与字节重载相同 |
| `readLine(timeoutMs = 0)` | →`string` | 读到下一个`\n`为止（含），去掉它（及它前面的`\r`）并返回该行；流在行中间结束时返回残缺的行，流结束且什么都没读到时返回空串——空行与输入结束在这里无法区分（`string`没有`null`值，见§4.3），输入结束的信号是`read`返回`0` |
| `readExactly(count, timeoutMs = 0)` | →`byte[]` | 读满恰好`count`个字节到新数组；流提前结束抛`SocketException`——按`Content-Length`分帧的正文就该用它 |
| `shutdownWrite()` | — | 半关闭：向对端发送流结束标记，读端保持打开（见下） |
| `close()` | — | 关闭连接；幂等。对端停泊中的读者会看到流结束 |
| `isClosed()` | →`bool` | 这个句柄是否已关闭 |
| `peerAddress()`/`peerPort()` | →`string`/`int` | 远端 |
| `localAddress()`/`localPort()` | →`string`/`int` | 本地端 |
| `setNoDelay(on)` | — | 关闭Nagle合批，用于对时延敏感的协议 |
| `setKeepAlive(on)` | — | 启用TCP保活探测 |
| `setSendBufferSize(bytes)`/`setRecvBufferSize(bytes)` | — | 请求套接字缓冲区大小 |

## 流结束与半关闭

`read`返回`0`就是字节流的有序结束：对端调用了`shutdownWrite`或`close`。任何一边都可以只用`shutdownWrite`关掉自己这一侧的写——经典的请求/应答形态：客户端宣告"请求写完"，同时还在读应答。

单进程里的echo往返——绑定、连接、接受、各一行、半关闭、自退出：

```nlang
import io;
import thread;
import net;

class Echo {
    public TcpStream conn;
    public void run() {
        string line = conn.readLine(5000);
        conn.write("echo:" + line + "\n");
        conn.close();
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    Echo e = Echo();
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    e.conn = srv.accept(5000);
    Thread t = thread.spawn(e.run);

    cli.write("morning\n");
    cli.shutdownWrite();              //半关闭：写完了
    string reply = cli.readLine(5000);
    io.print("server says: " + reply + "\n");
    cli.close();
    thread.join(t);
    srv.close();
    return 0;
}
```

同一形态放大到服务器规模——accept循环每连接派生一个虚拟线程、一次上千个——见随包发布的`examples/net_echo_server.n`与`examples/net_conc_bench.n`；`examples/net_http_mini.n`在其上叠了按行分帧与`Content-Length`正文。
