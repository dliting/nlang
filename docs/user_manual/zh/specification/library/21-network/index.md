> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 21. 网络库

网络库是一个标准库包`net`：传输控制协议（TCP，Transmission Control Protocol）的流与监听器、用户数据报协议（UDP，User Datagram Protocol）的数据报、以及互联网控制报文协议（ICMP，Internet Control Message Protocol）的回声请求。每个阻塞方法的停泊与调度语义都在第10章：被阻塞的套接字调用停泊发起调用的虚拟线程并让出它的载体，因此成千上万个并发连接可以同时等在套接字上——这个数目不受载体线程数的限制，单载体下同样成立。§21.1固定包形态、表面与共享约定；§21.2定义TCP类；§21.3定义`UdpSocket`；§21.4定义ICMP面。

## 21.1 net包

`net`是标准库包（§3.9.4）：`import net;`把它带入作用域。包名是单段的（§3.3）。它的声明是标准库位于`stdlib/net.n`的`.n`源码；它的`native`成员（§7.5）由随工具链发布的原生模块`nlang_net`实现——与`tensor`的内核（§19.1）同一种第三方机制——在首次调用时惰性加载（§7.5）。虚拟机本身不依赖该模块：从不触碰套接字的程序从不加载它。该包声明类`TcpListener`、`TcpStream`、`UdpSocket`与`IcmpSocket`，自由函数`ping`，以及异常子类`SocketException`与`TimeoutException`（§8.3）；各类经这次导入以普通类的身份进入作用域，`ping`经包限定符调用（`net.ping(host)`）。名为`net`的用户包会与标准库撞名，被重复包规则（§11.1）拒绝。

该包的表面：

- `net.ping(host, timeoutMs = 5000)`向`host`发送ICMP回声请求直到有一个得到应答，以毫秒返回往返时延（§21.4）。
- `TcpListener`——bind、accept、close（§21.2）。
- `TcpStream`——connect、read、write、readLine、readExactly、shutdownWrite、close与选项设置方法（§21.2）。
- `UdpSocket`——bind、sendTo、recvFrom、connect、send、recv与广播、组播、缓冲区大小选项（§21.3）。
- `IcmpSocket`——bind、sendTo、recvFrom、close（§21.4）。

### 停泊与超时

每个阻塞方法——`accept`、`read`、`readLine`、`readExactly`、`write`、`send`、`sendTo`、`recv`、`recvFrom`、`connect`、`ping`——停泊发起调用的虚拟线程，直到操作完成或截止时刻到来；载体在此期间运行其他上下文（§10.1）。以主机名给出地址的方法——`bind`、`connect`、`sendTo`、`ping`——在解析名字期间同样停泊：解析在运行时的解析线程池上执行，缓慢的查询不会占用任何载体（§10.1）。`timeoutMs`参数固定这次调用最多等多久：`0`（缺省）不设截止地等待，正值最多等待那么多毫秒。以主机名给出的调用——`connect`、`sendTo`——截止同样覆盖名字解析。等待在截止时刻结束抛`TimeoutException`；套接字保持打开、仍可用，之后的调用可能成功——唯一的例外是载荷中途超时的`write`：部分字节可能已经交给内核，调用方无法得知送出了多少（协议没有可续帧时，关闭重连）。

### 每个方向一个操作

一个套接字同一时刻每个方向只允许一个进行中的操作：一个上下文正在该套接字上读（无论已停泊还是仍在调用中）时，另一个上下文对同一套接字的第二次`read`抛`SocketException`（`another operation is in progress on this socket`），而不是与第一个争抢字节。惯用形态是每个连接一个上下文在读、需要时另一个在写——读永远不会把一个流拆给两个读者。

### 异常

`SocketException`与`TimeoutException`由包源码声明为派生自`IOException`的`const`类（§8.3、§20）：每个net失败都是一个`IOException`，所以`catch (IOException e)`把网络错误与文件错误一起覆盖，`catch (SocketException e)`或`catch (TimeoutException e)`收窄到网络。两者都带通常的`message`与`cause`字段（§8.3）；平台级失败会把操作系统的错误文本追加进消息。`SocketException`还带一个`errorCode`字段：失败来自操作系统时是它的错误号（Windows上是Winsock错误码——`10061`连接被拒、`10048`地址已被占用、`11001`主机名未找到），没有时为`0`（参数错误、内部状态）。程序按号码分支——消息文本随操作系统语言，号码不随。

### 字节与字符串

套接字载荷是`byte[]`数组（§4.10）。`io`包的`io.toBytes`与`io.fromBytes`（§14.5）在文本与原始Unicode转换格式（UTF-8）字节之间转换，供协议工作使用；`TcpStream.write(string)`是替你编码的便捷重载。`read`与`recvFrom`报告实收长度——只转换或转发那么多字节，永远不是整个缓冲。

```nlang
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    int rc = 0;
    if (srv.localPort() <= 0) { rc = 1; }   //端口0由系统分配
    srv.close();
    return rc;
}
```
