> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 21.3 UdpSocket类

用户数据报协议（UDP，User Datagram Protocol）是无连接、无序、尽力交付的数据报协议：每次发送是一个数据报，每次接收取走一个完整数据报，不保证送达。`UdpSocket`覆盖两种角色——未连接的套接字用显式地址收发（`sendTo`/`recvFrom`），已连接的套接字对一个对端用普通的`send`/`recv`。停泊与超时约定、每方向一个操作的规则与关闭句柄规则都在§21.1。

| 成员 | 形态 | 语义 |
|---|---|---|
| `UdpSocket.bind(host = "0.0.0.0", port = 0)` | 静态→`UdpSocket` | 绑定本地端点；端口`0`由系统分配，经`localPort()`读回 |
| `sendTo(data, host, port, count = -1, timeoutMs = 0)` | →`int` | 发送一个数据报——`data`的全部或前`count`个字节——到`host:port`；解析`host`期间以及发送缓冲区满时停泊，由`timeoutMs`约束（§21.1） |
| `recvFrom(buf, out peerAddress, out peerPort, timeoutMs = 0)` | →`int` | 取走下一个数据报，经`out`参数报告它的发送者；返回实收字节数；停泊直到有数据报到来或截止时刻到来。零长度的`buf`是参数错误（`SocketException`）——`0`保留给真正的空数据报 |
| `connect(host, port)` | — | 把套接字关联到一个对端；此后`send`/`recv`不需要地址，且只有该对端的数据报会到达 |
| `send(data, count = -1, timeoutMs = 0)` | →`int` | 向关联的对端发送一个数据报；套接字未连接时抛`SocketException`；发送缓冲区满时停泊，由`timeoutMs`约束（§21.1） |
| `recv(buf, timeoutMs = 0)` | →`int` | 从关联的对端取走下一个数据报并返回它的字节数——`0`是空数据报而非流结束：UDP没有可结束的流 |
| `setBroadcast(on)` | — | 允许向广播地址发送（`SO_BROADCAST`） |
| `joinMulticast(group)`/`leaveMulticast(group)` | — | 加入或离开组播组；接收接口是套接字的绑定地址（通配绑定即选系统默认接口） |
| `setMulticastTtl(ttl)` | — | 发出组播数据报的生存时间（`1`只留在本地链路） |
| `setTtl(ttl)` | — | 发出单播数据报的生存时间 |
| `setSendBufferSize(bytes)`/`setRecvBufferSize(bytes)` | — | 请求套接字缓冲区大小 |
| `localAddress()`/`localPort()` | →`string`/`int` | 本地端点 |
| `peerAddress()`/`peerPort()` | →`string`/`int` | 关联的对端；套接字未连接时抛`SocketException` |
| `close()` | — | 关闭套接字；幂等 |

## 数据报，不是流

收到的数据报拷贝进`buf`，比`buf.length`长时截断——多余的字节被丢弃，这是套接字接口的标准行为。按协议允许的最大数据报来定接收缓冲区的大小，并把返回值当实收长度用：只转换或转发那么多字节。

`recvFrom`运行时请求早已在队列里，所以这次交换不需要第二个线程；真正的服务器停在`recvFrom`上，从`out`参数取发送者并应答：

```nlang
import io;
import net;

int main() {
    UdpSocket srv = UdpSocket.bind("127.0.0.1", 0);
    UdpSocket cli = UdpSocket.bind("127.0.0.1", 0);
    cli.sendTo(io.toBytes("time?"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[64];
    string from;
    int port = 0;
    int n = srv.recvFrom(buf, out from, out port, 5000);
    cli.close();
    srv.close();
    if (n == 5 && from == "127.0.0.1") { return 7; }
    return 1;
}
```

随包发布的`examples/net_udp_echo.n`展示服务器形态：停泊，从`out`参数读发送者的地址与端口，把收到的字节原样发回那个端点。
