# 通用网络库设计(net 包)

- **文档定位**:nlang 通用网络库的立项级概要设计(Spec)。定义功能面、脚本层 API、
  native 分层、并发/停泊集成、错误模型、测试与验收;实施计划由 writing-plans 另文产出。
- **输入**:用户立项要求(高并发、TCP/UDP/ICMP、不向下兼容、将来 HTTP 构建其上、
  API 方便调用且符合现代网络库命名习惯、配套高并发示例);主流网络库联网调研
  (第三节,带出处);仓库基础设施勘察(标准库打包、native 模块通道、调度器停泊机制);
  2026-11-06 实施中架构裁定(见文末修订记录)。
- **裁定原则**:不向下兼容;选最贴合 nlang 虚拟线程模型的最合适方案;KISS,
  机制层复用既有调度器停泊先例,不为网络另造异步体系。
- **状态**:修订 r1(2026-11-06 裁定后),实施中。

## 一、目标与范围

**v1 做**:

1. `net` 标准库包:TCP(监听/连接/流式读写)、UDP(单播/广播/组播)、
   ICMP(ping 便利面 + IcmpSocket 报文面)。
2. 高并发:阻塞风格 API 跑在虚拟线程上,百万级停泊连接不受载体数限制
   (验收 200 并发连接入 e2e、1000 入示例门、10000 入本地基准)。
3. 超时:每个阻塞操作带可选 `timeoutMs`,到期抛 `TimeoutException`。
4. DNS:主机名透明解析(专用解析线程,不占载体)。
5. 示例族:echo 服务器/客户端、最小 HTTP 服务器、UDP echo、ping、
   千并发压测,全部进 e2e 示例门。
6. 为 HTTP 层预留地基:`readLine`/`readExactly` 分帧便利面、字节缓冲、
   全写语义;TLS 不在 v1。

**v1 不做(非目标)**:TLS/HTTPS、WebSocket、HTTP 协议本身、原始 IP 收发、
socket 选项的完整透传(只暴露常用面)、`select`/多路复用等待原语暴露给脚本
(将来可基于 `Future.selectAny` 扩展)、Linux 后端实装(留移植任务,接口预留)、
IPv6 全功能(v1 支持字面量与双栈监听,测试限于 `::1` 回环)。

## 二、关键事实(勘察结论,决定架构走向)

1. **阻塞 native 调用(regBlocking)并发槽位 ≤ 载体数**(nvm 缺省 4,可调):
   每次在飞阻塞 native 占用一个载体 OS 线程,超出排队。因此 socket 阻塞操作
   **绝不能**走 regBlocking——1 万连接各停在一个 `recv` 上时只有 4 个能真正
   等在内核里。必须走 Channel/Mutex 同款路线:挂起虚拟线程 + 专用线程唤醒,
   停泊数不受载体数限制。
2. 带每实例 C++ 状态的句柄类只有 VM 内建 intrinsic 通道(通道 B)有先例
   (Thread/Mutex/Channel/Future);host dll 通道(通道 A)只验证过自由函数。
   (修订 r1 推翻本条约束:通道 A 先例已扩展——io/math/fs/tensor 均为
   src/native/ 下 DLL 模块,与第三方库同一加载机制、按名自动加载;net 句柄
   改为脚本类持 int,不再需要 VM 侧句柄类;模块的每执行器状态由 ABI v11
   的宿主托管状态槽提供。)
3. 调度器已有 `ParkSignal`/`RequestPark` 挂起机制与 ParkedTimer/Join/Mutex/
   ChanRecv 等停泊状态族;定时器专用线程(`ParkForTimer` 惰性启动、到期迁移)
   是"外部事件源唤醒停泊上下文"的现成模板。
4. 仓库无任何既有网络代码;Windows 需链 `ws2_32`(ICMP ping 另需 `iphlpapi`)。
5. 异常树在 `stdlib/lang.n`(Exception→IOException 等 8 类);库级子类先例:
   `ChannelClosedException` 带 native ctor(VM 侧构造抛出),纯脚本子类也有先例。
6. `byte`/`ubyte` 与 `Array<T>` 存在(RTK_Byte 族);string 是字节容器
   (`io.readFile` 以 string 返回字节),但 string↔byte[] 无现成转换方法,
   且 Phase 11 已冻结 string 方法面——转换助手放 net 包内,不动 string。
7. 函数/方法重载受支持(§7.3),`write(byte[])` 与 `write(string)` 可共存。
8. 新增 `stdlib/net.n` 无需改根 CMake(GLOB 自动进 npkg/install);
   符号索引测试钉死 stdlib 函数计数与命名空间列表,须同步;手册"六个包"改七个。
9. e2e 全部 hermetic,无真实网络先例;手册围栏代码会被 docs 构建真编译真运行
   ——net 示例必须绑回环 + 端口 0 且自行退出。

## 三、调研结论与出处

| 结论 | 出处 |
| --- | --- |
| Go 运行时 netpoller:网络 fd 一律非阻塞,阻塞操作把 goroutine `gopark` 挂起并注册进 epoll/kqueue/IOCP,专用轮询路径收割就绪事件后唤醒 G——阻塞式 API 高并发的工业标准答案,与 nlang 虚拟线程模型同构 | [Go runtime netpoll 解析(CSDN,2026-09)](https://blog.csdn.net/csdnUser12/article/details/163554663)、[netpoll 源码解析(微博,2024)](https://weibo.com/ttarticle/p/show?id=2309404613112967070237) |
| JDK 21 虚拟线程(JEP 444):JDK 改造 `java.net.Socket` 内部为非阻塞+poller,虚拟线程阻塞在网络读上即从载体卸载;同步阻塞 API + M:N 调度即可达近异步吞吐——与 nlang"阻塞式等待即全部等待形态"完全同构的先例。注意其反面清单:`Selector.select()`(用户自跑事件循环)不在虚拟线程友好之列——runtime 内置 readiness 整合才是正路 | [JEP 444](https://openjdk.org/jeps/444)、[Java 21 虚拟线程机制综述(掘金,2026)](https://juejin.cn/post/7677771411977044003) |
| libuv(Node.js/Neovim/Julia 底座):事件循环 poll 阶段以 epoll/kqueue/IOCP 收割就绪,而 **DNS(getaddrinfo)与文件 IO 走线程池**(缺省 4 线程)——regular file 不可 poll(恒就绪)、getaddrinfo 无 fd 可 poll;runtime 的 readiness 设施按 fd 通用,网络库只是第一个消费者,管道/进程 stdio 等同享 | [libuv 事件循环阶段与双轨(知乎,2019)](https://zhuanlan.zhihu.com/p/86249296)、[libuv 线程池/uvqueuework 用途](http://news.558idc.com/35807.html) |
| Windows 上 `SOCK_RAW/IPPROTO_ICMP` 自 XP SP2 起需管理员(否则 WSAEPERM/10013);非特权 ping 的正规路线是 IP Helper API `IcmpCreateFile`/`IcmpSendEcho2`(自动处理校验和与 ID/序列) | [Windows ICMP 权限与 IcmpSendEcho(php.cn,2026-02)](https://www.php.cn/faq/2117702.html)、[SO 讨论:ICMP raw 仅限管理员](https://stackoverflow.com/questions/745602) |
| Linux 非特权 ICMP:`SOCK_DGRAM/IPPROTO_ICMP` + `net.ipv4.ping_group_range`(内核 3.0+;Fedora/Ubuntu 现代发行版默认放开);内核自动填 ID 与校验和,无 IP 头 | [icmp(7) 转述(Stack Overflow)](https://stackoverflow.org.cn/questions/1189389)、[Fedora ping_group_range 变更说明](https://fedoraproject.org/wiki/Changes/EnableSysctlPingGroupRange) |
| Windows 多路复用:select 受 FD_SETSIZE 1024 限制,WSAPoll 有已知兼容性问题(非阻塞 connect 可写性误报等);wepoll 以 IOCP 实现 epoll 兼容 API(2 个文件,MIT,线程安全,支持数十万 fd),是 reactor 模型在 Windows 的成熟落点 | [wepoll 项目解析(CSDN,2025-11)](https://blog.csdn.net/gitblog_00754/article/details/155084876) |
| 脚本语言网络库 API 粒度先例:Node.js(net/dgram 连接对象+事件)、LuaSocket(连接对象+超时参数)、Go(net.Dial/Conn.Read 阻塞流)——主流都收敛到"连接对象 + 阻塞/等待方法 + 超时参数",不向脚本层暴露 fd/epoll | Node.js/LuaSocket/Go 官方文档 |

## 四、架构裁定

| # | 决策 | 要点与理由 |
| --- | --- | --- |
| D1 | **同步阻塞风格 API,无 async/await/回调** | 与 nlang 并发模型(阻塞式等待即全部等待形态,0.10.3 裁定)一致;Go/JDK 21 同构先例(第三节)。高并发来自"每连接一虚拟线程 + 停泊不占载体",不来自 API 染色 |
| D2 | **实现通道 = VM 外 native 模块 `nlang_net`(通道 A,同 io/fs/math/tensor 先例)** | 用户裁定(2026-11-06):网络库不并入 VM,构建为独立、类第三方库的模块,VM 不依赖网络库运行。仓库同型先例:src/native/ 下四模块仅依赖 NativeHost ABI、不链接 nlang_vm,加载器按名自动加载。句柄 = 脚本类内 int 字段;公开方法一律脚本方法,委托模块注册的内部 namespace native(ABI 无对象字段访问,receiver 只是对象句柄;同 tensor fullI 内部 native 先例)。不需要 VM 侧句柄类/intrinsic 注册,VM 内核零增大;性能不敏感面(readLine/readExactly/write(string)/connect 重试循环等)按裁定第 3 点归脚本 |
| D3 | **非阻塞 socket + VM 专用 IO 轮询线程 + `ParkedIo` 停泊,经 ABI v11 暴露给模块** | 轮询线程属 VM 运行时基础设施(裁定第 2 点,已提交):复刻定时器线程模板惰性启动。模块 native 经 host 回调 `parkForIo(fd, events, deadlineMs, hasDeadline)` 登记兴趣后解卷,唤醒后 VM 以原参数重入该 native 调用——参数驻留脚本帧,绝对截止点随参数穿越重入,天然稳定;`nowMs` 提供与轮询同域的单调毫秒。阻塞操作 = 尝试非阻塞 syscall → would-block 则 parkForIo → 收割就绪后重入重试。停泊上下文数只受内存限制。定位:轮询设施是 VM 调度器的**通用 IO readiness 设施**(按 fd,不限 socket)——管道、进程 stdio 等持 fd 的原生库复用同一 `parkForIo`,net 只是第一个消费者(libuv/Go 同样按 fd 通用);regular file 不可 poll(恒就绪),将来 fs 异步走线程池(libuv fs、Go、Loom FileChannel 同此);定时器线程与轮询线程合一(poll 超时=最近截止,libuv 模式)为后续简化方向,v1 维持两线程 |
| D4 | **轮询后端:Windows 用 vendored wepoll;Linux epoll 预留** | 第二节 WSAPoll 兼容性陷阱;wepoll=IOCP 之上的 epoll 兼容层,与将来 Linux 后端同一套 reactor 语义,代码路径合一。src/3rdparty 已有 vendoring 先例(tinyxml2/libvterm),BSD-2-Clause 2 文件 |
| D5 | **DNS 解析与 ICMP echo 的阻塞卸载走 VM 通用原生工作线程池;完成经自管道 ack 唤醒** | `getaddrinfo`/`IcmpSendEcho2` 是阻塞调用,不在载体上跑、不占 regBlocking 槽位。修订 r1 补注 4(用户裁定 2026-11-06):工作线程池是通用并发基础设施(fs 常规文件 IO、HTTP 层等后续同需,libuv 线程池同构),不随 net 模块私有——引擎归 VM(并发库的原生引擎层即 VM:载体/Channel/Mutex 引擎皆在此,模块只依赖 ABI、彼此不能共享代码),经 ABI v11 `submitWork(fn, arg, ackFd, priority)` 暴露给原生模块;完成结果写入模块作业表,worker 向 socketpair 自管道写一字节——停泊在管道读端的上下文被 VM 轮询线程按普通 fd 兴趣收割唤醒,无需 VM 侧专用唤醒通道。池有空闲 worker 时慢解析(可达 5s)不延迟 ping:按 priority 两级队列(libuv `uv_queue_work` 优先级先例);priority 只排定队列次序、不抢占已在跑的调用——满载(全部 worker 被慢解析占用)时排队的 ping 受排队影响。Linux 的 ICMP 走 socket+轮询线程,无需工作线程。脚本面便利类(concurrent 包 ThreadPool)随首个脚本级消费者再议 |
| D6 | **ICMP 双路线**:ping 便利面按平台走非特权路线;IcmpSocket 报文面 Windows=raw(需管理员,错误信息明示)、Linux=dgram | 第三节权限结论。ping() 在 Windows 用 `IcmpSendEcho2`(免管理员),Linux 用 DGRAM ICMP socket;IcmpSocket 在 Windows raw 发送时由 native 自动补 ICMP 校验和 |
| D7 | **数据面 = `byte[]` 缓冲 + Go 式读写语义** | `read(buf)` 至多填 buf.length,返回字节数,0=EOF;`write(data, count=-1)` 写前 count 字节、全写(内部循环),返回写入字节数(r1补注3 加 count 默认参,echo 回发前 n 字节便利)。string 经 `io.toBytes/fromBytes` 进出(不动冻结的 string 方法面;字节助手归 io 包,r1补注5) |
| D8 | **超时 = 每阻塞操作 `timeoutMs = 0`(0 为无限),到期抛 `TimeoutException`** | 超时截止与 IO 兴趣同时登记;轮询线程的等待上限取最近截止(复刻定时器线程"睡到最近到期"模式),到期与就绪先到先唤醒,唤醒后复查决定返回或抛。不耦合定时器线程,避免双线程竞态 |
| D9 | **异常 = `SocketException : IOException` 与 `TimeoutException : IOException`,纯脚本类** | 修订 r1:模块在 VM 异常 ABI(五固定族)之外,不能构造 VM 注册的异常类;native 返回错误码,脚本层方法映射抛出 SocketException/TimeoutException(脚本 ctor 委托基类 Exception;纯脚本异常子类有先例)。平台错误文本经模块内部 native 读取 last-error 拼入消息。catch IOException 覆盖面不变;DNS 失败/拒连/重置均 SocketException,消息带主机端口上下文 |
| D10 | **地址 = 字符串表示**("192.168.1.1"、"[::1]"、"example.com");`bind(port)` 默认 "0.0.0.0";端口 0 自动分配经 `localPort()` 读回 | 脚本层不引入 Endpoint 值类型(YAGNI;Go 的 string 地址面最省学习成本);解析发生在 connect/bind/ping 内部,透明 |
| D11 | **每 socket 每方向同时至多一个阻塞操作;close() 唤醒停泊者并使其抛 SocketException** | 与 Go"并发读写允许"不同,nlang 取确定性:第二个同方向停泊者直接 SocketException("concurrent operation"),避免未定义竞态;close 语义对齐 Channel close 的唤醒先例 |
| D12 | **资源生命周期 = 显式 `close()`(VM 无 finalizer);执行器收尾兜底关闭** | 修订 r1:脚本类持 int 句柄(代际编码防 ABA);socket 槽表/作业表存于模块的宿主托管每执行器状态(ABI v11),执行器析构时状态随之析构→关闭未关 fd;进程退出由 OS 回收。`WSAStartup`/`WSACleanup` 移入模块(进程级 once_flag+atexit);模块链接 ws2_32+iphlpapi,VM 去 iphlpapi、保留 ws2_32(wepoll 内部用 winsock 错误报告) |
| D13 | **通用 IO 设施归 `io` 包,不为 net 另立门户** | 用户裁定(2026-11-06):通用底层设施补充到现有系统 package。ABI v11 的 `nowMs`/`parkForIo` 是通用设施:nlang_io 模块注册脚本面 `io.nowMs()`(单调毫秒)与 `io.parkForIo(fd, events, deadlineMs)`(fd 停泊,供一切持 fd 的原生库及将来管道/进程复用);net 的 C native 直接在 C 层用 host 回调,net.n 仅留一个内部 `_nowMs` 供脚本层算初始截止(预置包互不 import,各包自足)。r1补注5 扩展:通用 UTF-8 字节转换 `toBytes`/`fromBytes` 一并移入 io 包——io 已拥有 ByteStream/FileStream 字节容器类族,fs/process/HTTP 将来复用;net 公开面为 byte[] 基、不依赖它们,net.n 脚本便捷面改用模块内部 `_toBytes`/`_fromBytes` |

## 五、脚本层 API 面(net.n 权威声明草案)

下文的 `native` 标注是语义标记(实现位于 nlang_net 模块):公开方法实际为
脚本方法,委托内部 `_xxx` native 并把错误码映射为异常(D2/D9),签名形态
一致。

```nlang
// net.n — 网络通信包。需显式 import net;

// ---- 异常(纯脚本类;native 返错误码,脚本方法映射抛出) ----
const class SocketException : IOException {
    // 脚本 ctor,委托基类 Exception(message, cause)(语法以编译器既有
    // 基类构造器委托形态为准)。
    public SocketException(string message, Exception cause = null);
}
const class TimeoutException : IOException {
    public TimeoutException(string message, Exception cause = null);
}

// ---- 字节助手已移入 io 包(io.toBytes/io.fromBytes,r1补注5/D13):
//      通用 UTF-8 字节转换非网络专属;net.n 脚本便捷面用模块内部
//      _toBytes/_fromBytes(预置包互不 import) ----

// ---- ICMP 便利面 ----
// 发送 Echo Request 并等待 Echo Reply,返回往返时延毫秒。
// 超时抛 TimeoutException;主机无法解析或不可达抛 SocketException。
// Windows 走 IcmpSendEcho2(免管理员),Linux 走 DGRAM ICMP socket。
native int ping(string host, int timeoutMs = 5000);

// ---- TCP ----
class TcpListener {
    // 监听 port(所有接口)或 host:port;port=0 由系统分配,经 localPort() 读回。
    public static native TcpListener bind(int port);
    public static native TcpListener bind(string host, int port);
    // 接受下一个连接;停泊直至有连接或超时。
    public native TcpStream accept(int timeoutMs = 0);
    public native string localAddress();
    public native int localPort();
    public native void close();
}

class TcpStream {
    // 解析 host(停泊于解析线程)并按序尝试地址,非阻塞 connect;
    // timeoutMs 覆盖解析+连接全程。
    public static native TcpStream connect(string host, int port, int timeoutMs = 0);
    // 至多读满 buf.length 字节;返回读到的字节数,0 表示对端已关闭(EOF)。
    public native int read(byte[] buf, int timeoutMs = 0);
    // 写 data 前 count 字节(count=-1 为全数组);内部循环直至全部发出;
    // 返回写入字节数。
    public native int write(byte[] data, int count = -1);
    // 读一行(以 '\n' 结尾,返回不含行尾的文本;'\r' 被剥离)。
    // 对端在行结束前关闭返回已积累文本;零字节且 EOF 返回 null。脚本层实现。
    public string readLine(int timeoutMs = 0);
    // 读满 count 字节(HTTP 定长体分帧);EOF 早至抛 SocketException。脚本层实现。
    public byte[] readExactly(int count, int timeoutMs = 0);
    // 文本便捷面(重载,§7.3):write(_toBytes(s)),_toBytes 为模块内部 native。脚本层实现。
    public int write(string s);
    public native void close();
    // 半关闭:发出 FIN 告知对端写端结束,读方向保持可用(HTTP/1.0 式
    // "发完请求再读响应"等地基)。close 之前的可选前置步。
    public native void shutdownWrite();
    public native bool isClosed();
    // 选项面(设置即刻生效;失败抛 SocketException)
    public native void setNoDelay(bool on);        // Nagle 开关,默认 OS 行为
    public native void setKeepAlive(bool on);
    public native void setSendBufferSize(int bytes);
    public native void setRecvBufferSize(int bytes);
    public native string peerAddress();
    public native int peerPort();
    public native string localAddress();
    public native int localPort();
}

// ---- UDP ----
class UdpSocket {
    // port=0 自动分配;host="0.0.0.0" 收所有接口。
    public static native UdpSocket bind(string host = "0.0.0.0", int port = 0);
    public native int sendTo(byte[] data, string host, int port, int count = -1);
    public native int recvFrom(byte[] buf, out string peerAddress, out int peerPort,
                               int timeoutMs = 0);   // 数据报截断则丢弃多余字节(POSIX 语义)
    // 关联模式:connect 后可用 send/recv 与 peerAddress()/peerPort()。
    public native void connect(string host, int port);
    public native int send(byte[] data, int count = -1);
    public native int recv(byte[] buf, int timeoutMs = 0);
    public native void setBroadcast(bool on);        // 发 255.255.255.255 前置
    public native void joinMulticast(string group);  // group 如 "239.1.2.3"
    public native void leaveMulticast(string group);
    public native void setMulticastTtl(int ttl);
    public native void setTtl(int ttl);              // 单播跳数上限(IP_TTL)
    public native string localAddress();
    public native int localPort();
    public native void close();
}

// ---- ICMP 报文面(自定义类型/代码/载荷;Echo 便利面用 ping()) ----
class IcmpSocket {
    // Windows:SOCK_RAW/IPPROTO_ICMP,需管理员,否则 bind 抛
    // SocketException("raw ICMP requires administrator privileges on Windows")。
    // Linux:SOCK_DGRAM/IPPROTO_ICMP(免特权,受 ping_group_range 约束)。
    public static native IcmpSocket bind(string host = "0.0.0.0");
    // 发送完整 ICMP 消息(类型/代码/校验和/ID/序列/载荷);
    // Windows raw 下校验和字段为 0 时由 native 自动计算。
    public native int sendTo(byte[] icmpMessage, string host);
    // 收到一条 ICMP 消息,返回来源地址。
    public native int recvFrom(byte[] buf, out string peerAddress, int timeoutMs = 0);
    public native void close();
}
```

设计要点:

- **修订 r1 实现切分**:块内 `native` 标注自此仅表示"实现落在 nlang_net
  模块",不再是 VM intrinsic。公开方法最终均为脚本方法,委托内部 namespace
  native(如 `_tcpBind`/`_tcpRead`,命名后缀形态同 tensor fullI 先例)并把
  错误码映射为异常;用户可见面(名称/签名/语义)不变。
- **命名对齐现代惯例**:connect/bind/accept/read/write/send/recv/sendTo/recvFrom/
  close/setXxx 均为 Go/Rust/C#/Node 的通用动词;nlang 侧仅做 camelCase/PascalCase
  风格化。包名 `net`(Go/Rust std::net 同款)。
- **每连接一虚拟线程**是服务器惯用法;nlang spawn 限零参委托,处理器以
  "类实例字段携带连接 + 绑定方法"传递(示例示范,语言既有约束)。
- `readLine`/`readExactly`/`write(string)` 是**脚本层方法**(net.n 内以
  read/write 组装),体现"native 原语 + 脚本组装"的分层,也直接服务将来 HTTP。
- 无 getter 的选项(只 set 不 get)是刻意裁剪:v1 不提供查询面。

## 六、native 层结构

```
src/native/net/net_native.cpp        模块入口(NLANG_DEFINE_NATIVE_INIT)+ 字节助手/_nowMs/
                                     错误文本/作业等待等支持 native
src/native/net/net_tcp.cpp           TCP 内部 native(bind/accept/connect 尝试/read/write/选项)
src/native/net/net_udp.cpp           UDP 内部 native
src/native/net/net_icmp.cpp          IcmpSocket + ping 内部 native(echo 作业走 VM 工作线程池)
src/native/net/net_jobs.cpp          作业表 + socketpair 自管道 ack(工作线程池归 VM,
                                     经 submitWork 提交,见 VmWorkPool)
src/native/net/net_state.cpp         每执行器模块状态(宿主托管):socket 槽表/作业表/last-error
src/native/net/net_platform.h        平台抽象:create/bind/listen/connect/recv/send/
                                     setsockopt/ICMP 原语/socketpair(readiness 轮询归 VM 侧)
src/native/net/net_platform_windows.cpp  winsock2 + IcmpSendEcho2
src/native/net/net_platform_posix.cpp    POSIX 骨架(v1 编译通过即可,epoll 实装留移植任务)
src/3rdparty/wepoll/               vendored wepoll.c/wepoll.h(BSD-2-Clause,LICENSE 登记,仅编入 nlang_vm)
src/vm/IoPoller.h / IoPollerWindows.cpp / IoPollerPosix.cpp
                                   readiness 轮询抽象(wepoll/epoll):通用 fd 就绪,
                                   从 net 平台层拆出归 VM——模块不 poll,只经 parkForIo 登记兴趣
src/vm/VmSchedulerIo.cpp           IO 轮询线程(VM 基础设施,已提交):ParkForIo/兴趣表/
                                   收割唤醒/最近截止睡眠
src/vm/VmWorkPool.h / VmWorkPool.cpp 通用原生工作线程池(并发基础设施,per-调度器惰性
                                   起停,与轮询线程同模板):submitWork(fn, arg, ackFd,
                                   priority),worker 无 GIL 跑 fn,完成写 ackFd 一字节
include/nlang/vm/NativeHost.h      ABI v11:nowMs / parkForIo / moduleState / wakeIoWaiters / currentContextId / submitWork
```

- **模块接入**:`nlang_net` 进 `src/native/CMakeLists.txt` 的
  `NLANG_NATIVE_MODULES`(SHARED,仅依赖 NativeHost.h+STL,不链接 nlang_vm);
  全部 net native 走 regBlocking 通道(参数/结果由 VM 扎根暂存,GIL 释放后
  执行,parkForIo 内部自取 GIL)。首次调用任意 `net.*` 触发加载器按名自动
  加载(既有机制,io 同路)。
- **编译器侧零接入**:net.n 的 `native` 声明驱动 codegen(按名 CallNative +
  自动加载),无需 BindingTable 行、无需 `s_HandleClassNames` 条目——任务 4
  已加的 net 行与 4 个句柄类名按修订删除。
- **ABI v11(VM 基础设施暴露,通用)**:`nowMs()` 单调毫秒(与轮询线程截止同
  域);`parkForIo(fd, events, deadlineMs, hasDeadline)` 登记兴趣后解卷,唤醒
  后 VM 以原参数重入该 native 调用,重入时 parkForIo 直接返回(宿主以每上下文
  armed 标志区分首入/重入),调用方以 nowMs 复查截止后决定重试或超时——
  `fd < 0` 为纯截止停泊;`moduleState(tag, ctor, dtor)` 宿主托管每执行器状态
  槽(模块传唯一 tag 与构造/析构,首次调用构造、执行器析构时运行析构);
  `wakeIoWaiters(fd)` 注销该 fd 的兴趣并唤醒全部停泊上下文——模块 close
  路径专用(模块不 poll,够不到 VM 轮询器;宿主内部取 GIL);
  `currentContextId()` 返回执行当前 blocking native 调用的上下文 id(停泊/
  重入间稳定)——模块按方向登记停泊等待者需要调用方身份,同方向并发停泊
  拒绝(D11)据此判别(实施计划裁定 R14);`submitWork(fn, arg, ackFd,
  priority)` 把阻塞函数提交 VM 通用原生工作线程池(per-调度器惰性起停,与
  轮询线程同模板;worker 无 GIL 跑 fn,完成向 ackFd 写一字节;priority
  0 常规/1 高,两级队列)——通用并发基础设施,net 的 DNS/ICMP 是第一个消费
  者,fs 常规文件 IO 后续复用(D5)。脚本面经 io 包暴露(D13)。
- **通用设施盘点(r1补注5,语言/工具链/标准库整体视角)**:

  | 设施 | 归属 | 第一消费者 | 将来复用/提取点 |
  | --- | --- | --- | --- |
  | IO 停泊 `io.nowMs`/`io.parkForIo` | VM 调度器引擎 + io 包脚本面 | net | 管道/进程 stdio、fs 异步 |
  | 原生工作线程池 `submitWork` | VM `VmWorkPool`(并发库的原生引擎层) | net DNS/ICMP | fs 常规文件 IO、HTTP;脚本面(concurrent.ThreadPool)随首个脚本级消费者——脚本闭包已有虚拟线程(thread.spawn),池的价值在卸载原生阻塞调用(C 函数指针层),脚本面与虚拟线程语义重复,非缺漏 |
  | 字节转换 `io.toBytes`/`io.fromBytes` | io 包(nlang_io native) | net 用例的用户便捷面 | fs、process、HTTP、crypto |
  | 每执行器模块状态 `moduleState` | VM ABI | net | 一切有状态 native 模块 |
  | 兴趣注销唤醒 `wakeIoWaiters`、上下文身份 `currentContextId` | VM ABI | net | 一切按 fd 停泊的模块 |
  | 自管道 ack(socketpair) | net 平台层(v1) | net 作业 ack | 提取点=fs/process 成为第二消费者时(模块间不能共享代码,约 10 行平台助手,容忍暂态重复) |
  | 流分帧 `readLine`/`readExactly` | net.n 脚本(v1) | net/HTTP 地基 | io 流类族(ByteStream/FileStream)随其自身消费者(日志读取、HTTP 文件服务)获得同名分帧;参考形态=net.n 脚本分帧 |
  | 定时器线程与轮询线程合一 | VM(后续) | — | libuv 单事件循环形态 |

  明确不建:缓冲池(syscall 直读写已够,性能实测需要再加)、Endpoint 值类型(D10)、
  事件循环脚本面(随 HTTP 层再议)、跨包 import(语言/工具链路线图项:跨包共享
  基类与助手依赖它,本项目范围外)。
- **socket 槽表(模块内)**:`NetSocketSlot` 注册表:
  `{fd, kind, closed, readWaiterCtx, writeWaiterCtx, acceptWaiterCtx, interestMask}`;
  int 句柄 = 槽 id+代际(防 ABA);close 置 closed、注销兴趣、唤醒停泊者、
  停泊者退出后才真正关 fd(防 fd 复用串扰);表存于 moduleState,执行器析构
  关闭全部未关 fd。
- **调度器(VM,已提交)**:`ContextState::ParkedIo` 单状态(方向记在槽上);
  `ParkForIo` 惰性启动轮询线程+两阶段握手防丢唤醒;收割线程 wepoll_wait
  (超时=兴趣表内最近截止,无兴趣长睡)→取 GIL→唤醒就绪/过期上下文;
  close/新兴趣 notify 重算等待上限。
- **GIL 纪律**:net native 运行于 regBlocking 通道=GIL 已释放,非阻塞 syscall
  不再跨载体串行(修订 r1 顺带解除原"GIL 下 syscall"瓶颈);轮询等待、
  getaddrinfo、IcmpSendEcho2 在无 GIL 的专用线程上跑;停泊/唤醒状态迁移在
  GIL 下完成。
- **GC 交互**:regBlocking 通道的暂存参数/结果由 VM 根登记表扎根(既有);
  停泊上下文帧(含 byte[] 缓冲引用)由既有跨挂起根集覆盖;句柄是普通 int,
  无新铸句柄扎根面。
- **WSA 生命周期**:模块内进程级 once_flag `WSAStartup` + atexit
  `WSACleanup`;模块链接 `ws2_32` + `iphlpapi`(Windows);VM 保留 `ws2_32`
  (wepoll 编入 VM、内部用 winsock 错误报告),去 `iphlpapi`。
- **source_size_guard**:500 行/文件、50 行/函数——按上表拆分 TU 预防。

## 七、关键数据流

**阻塞 read(无超时)**:
1. 模块 native(regBlocking 通道,GIL 已释放):查槽(closed→返回关闭码;
   同方向已有停泊者→返回并发违例码)→ 非阻塞 `recv` → 有数据拷贝进 buf
   返回字节数。
2. `WSAEWOULDBLOCK` → `host->parkForIo` 登记 readWaiterCtx + 兴趣 EPOLLIN
   并解卷(两阶段握手)→ 载体转去跑别的虚拟线程。
3. 轮询线程收割 EPOLLIN → 取 GIL → 唤醒该上下文 → VM 以原参数重入该
   native → 重试 recv(此时必不 would-block)→ 返回。EPOLLHUP/ERR →
   重入后按 EOF/错误码处理。
4. 脚本方法把码映射为异常(超时→TimeoutException,关闭/错误→
   SocketException,消息带平台错误文本)。

**connect(host, port, timeoutMs)**:脚本方法驱动:解析(提交模块解析池,
停泊于自管道读端)→ 逐地址非阻塞 connect,EINPROGRESS → 登记可写兴趣停泊 →
重入后 `getsockopt(SO_ERROR)` 判定 → 成功返回;失败尝试下一地址;全部失败
或截止已到 → 抛。timeoutMs 预算跨解析与连接两阶段(绝对截止点随参数穿越
重入,天然稳定)。

**超时竞态**:截止点与兴趣同时登记;轮询线程睡到最近截止;到期唤醒与就绪唤醒
谁先到都行,重入后复查"有数据?→返回;无数据且 nowMs 已过截止?→返回超时
码→脚本抛 TimeoutException"。复查以单调时钟为准,无丢唤醒(两阶段握手保证
park 完成前的唤醒不丢)。

**close 竞态**:另一上下文 close → 槽置 closed → 注销兴趣 → 唤醒停泊者 →
停泊者重入复查 closed → 返回关闭码 → 脚本抛 SocketException("socket
closed")。fd 在唤醒完成前不真正 close(引用计数:停泊者退出后才关),防
fd 复用串扰。

## 八、测试策略

- **e2e(hermetic 回环,命名 `stdlib_net_*.n` 入 manifest.txt)**:
  tcp echo(服务器 spawn + 客户端收发 + join)、udp echo + recvFrom 来源断言、
  readLine/readExactly 分帧、io.toBytes/fromBytes 往返(io 包用例)、connect 拒连抛
  SocketException、read 超时抛 TimeoutException(服务器收而不发)、
  connect("localhost") DNS 回环、close 唤醒停泊 reader、并发同方向拒绝、
  ping("127.0.0.1") 成功路径、IcmpSocket 非管理员 Windows 错误路径断言、
  组播 join 冒烟(239.x 回环)、千并发 echo(200 连接入 e2e;1000 入示例)。
  多载体变体加 `.flags --carriers=4`;GC 竞态按 gc-stress 纪律双形态跑
  (停泊 recv + 分配风暴)。
- **ctest(C++)**:socket 槽表生命周期(id 代际/close 唤醒/ABA——测试目标
  直接编译 src/native/net 源文件,模块不链接 nlang_vm,天然可单元测)、
  轮询线程启停与兴趣登记竞态、wepoll 集成冒烟;进程内宿主先
  `Runtime::StaticInit`(既有纪律)。blocking 通道 park 解卷/重入契约由 e2e
  钉死:`stdlib_io_park_now`(纯截止停泊+停泊让出载体,`.flags --carriers=1`)
  加各 net 停泊用例(read 超时、close 唤醒等)。
- **符号索引测试**:函数计数实施时钉死(net.n 内部 native 同样计数),
  命名空间列表插入 net(字母序);io 包计数 +4(nowMs/parkForIo/toBytes/fromBytes),net 自由函数计数先 −2(字节助手移入 io,任务 6 重写 net.n 时随内部 native 回升)。
- **docs snippet 审计**:手册围栏全部"绑 127.0.0.1 + 端口 0 + spawn + 自退出"。
- **门禁**:全套既有门禁(gate.sh 判决、ctest、e2e、IDE 树全量、docs strict、
  Debug 断言冒烟)。

## 九、示例设计(examples/,入 examples_manifest e2e 门)

| 示例 | 内容 | 演示点 |
| --- | --- | --- |
| `net_echo_server.n` / `net_echo_client.n` | 经典 echo:accept 循环 + 每连接 spawn 类方法 | 服务器惯用法、spawn 状态传递、EOF 收尾 |
| `net_http_mini.n` | 单文件:后台线程起迷你 HTTP 服务器(readLine 解请求行 + Content-Length 回固定体),主线程 connect 发 GET 打印响应 | readLine/readExactly 分帧、HTTP 分层地基 |
| `net_udp_echo.n` | UDP 请求/应答 + recvFrom 来源回发 | 数据报语义、out 参数取来源 |
| `net_ping.n` | ping 127.0.0.1 与 localhost,打印 RTT | ICMP 便利面、超时异常处理 |
| `net_conc_bench.n` | 同进程 1000 虚拟线程客户端 × 短消息 echo,打印总耗时与吞吐 | 高并发主张的可见证据(停泊不占载体) |

全部绑回环、端口 0、限时自退出(30s e2e 超时内);`net_conc_bench` 在
单载体下也必须完成(停泊唤醒不依赖多载体)。并发测试面取业内标准 echo
并发阶梯的代表性子集(200 入 e2e、1000 入示例门、10000 入本地基准),
吞吐输出按 ab/wrk 标准口径(并发数 × 总请求 → requests/sec + 总耗时),
便于与 Go/Node 同型实现横向对比;不引入 wrk 式外置负载工具(v1 负载端
即虚拟线程客户端,负载工具随 HTTP 层再议)。

## 十、集成点与文档同步

1. `stdlib/net.n`(权威声明;GLOB 自动打包)+ `examples/*.n` 五例;
   `stdlib/io.n` 增 `nowMs`/`parkForIo` 声明(D13)。
2. 模块:`src/native/net/` 九个 TU + CMake `NLANG_NATIVE_MODULES` 加 net +
   install bin/ + cpack;3rdparty wepoll 登记;模块链 ws2_32/iphlpapi。
3. VM/ABI:NativeHost.h v11(nowMs/parkForIo/moduleState/wakeIoWaiters/currentContextId/submitWork)+ VmExecutorNativeHost
   宿主回调实现 + blocking 臂 park 解卷处理 + VmSchedulerIo(已提交)+
   VmWorkPool 通用工作线程池(r1补注4);
   删除任务 4 的 VM 耦合(Intrinsics.h net ID、BindingTable net 行、
   s_HandleClassNames 四类、CacheBuiltinClassIndexes、VmExecutorIntrinsics
   级联、src/vm CMake net TU、iphlpapi 链接(ws2_32 保留——wepoll 需 winsock))。
4. 测试:tests/CMakeLists.txt(新 ctest 项)、e2e manifest 条目、
   test_symbol_index.cpp 计数与列表。
5. 手册:library/11-conventions.md"六个包"→七个;新 20-net 页(en+zh);
   io 页增 nowMs/parkForIo;mkdocs.{en,zh}.yml nav;specification/index.md;
   19-library-exceptions.md 增补两个异常类;docs/dev 本文档。
6. `CHANGELOG.md`/`CHANGELOG.zh-CN.md`;VERSION 0.12.6 → **0.13.0**(新功能次版本)。
7. 用户可见面同步:`docs/user_manual/{en,zh}/specification` 对应 spec 页
   (新增网络库规格节)。

## 十一、验收标准

1. 功能:TCP/UDP/ICMP 三协议 e2e 全绿;超时/关闭/并发违例错误路径全绿。
2. 高并发:`net_conc_bench.n` 1000 并发连接回环 echo 在 30s 内完成;
   本地基准(tools/,不入 ctest)10000 并发连接跑通并记录内存/耗时。
3. 质量:循环完善流程零 Medium+ 新发现;全套门禁绿;符号索引/守卫测试同步。
4. 文档:en+zh 手册页、示例、CHANGELOG、spec 页齐备;docs snippet 审计通过。

## 十二、风险与开放问题

| 风险/问题 | 处置 |
| --- | --- |
| wepoll vendoring(许可/构建集成/与 MSVC /W4 告警) | BSD-2-Clause 兼容;vendored 目标独立编译选项;若受阻退路 = WSAPoll 后端(接受已知怪癖)或直写 IOCP |
| ABI v11 park 解卷/重入(新机制) | 复用 VM 内建 intrinsic 停泊的解卷路径;blocking 臂以 park-pending 标志处置(不恢复帧、不消费结果;不经跨 DLL 异常传播,实施计划裁定 R7);e2e 以 close 唤醒 reader/超时/并发违例钉死;退路 = regBlocking 占载体(并发封顶载体数,10k 验收降级) |
| Windows 防火墙对监听 socket 弹窗 | 测试/示例一律绑 127.0.0.1(回环监听不触发防火墙提示);文档建议服务器明示绑址 |
| e2e 端口冲突 | 一律端口 0 + localPort() |
| IcmpSocket 在 Windows 非管理员不可用 | 明示错误信息;ping() 覆盖免特权主场景;e2e 断言错误路径 |
| 千并发 e2e 在慢机上超时 | e2e 入 200 连接,1000 在示例门(30s 超时余量实测后钉死) |

**自主裁定(用户离场,按"请自主决策"授权,理由如下)**:① 包名取 `net`——
Go/Rust 现代惯例,短且涵盖 TCP/UDP/ICMP 全面;② IcmpSocket 进 v1——四个
native 成本低,且"支持 ICMP 协议"的立项要求不止于 ping;③ `TimeoutException`
挂 `IOException` 下——网络超时是 I/O 超时,`catch IOException` 应覆盖之,
与 Java `SocketTimeoutException` 谱系同理,将来非网络超时若需要再议。

## 修订记录

- **r1(2026-11-06,实施中用户裁定)**:① 网络库不并入 VM,独立构建为
  `nlang_net` 模块(类第三方库),VM 不依赖网络库运行;② 通用运行时基础
  设施(IO 停泊)归 VM,其脚本面归 `io` 包;③ 性能不敏感面用 nlang 脚本
  实现;④ 底层实现跨平台;⑤ VM 本身可改(载体数、架构、语言面),不向下
  兼容、合理性优先——经比较弃"增载体"方案(10k 停泊占载体=10k OS 线程,
  违背 M:N 虚拟线程模型),采 ABI v11 停泊暴露。据此修订 D2/D3/D5/D9/D12/
  D13 与第二/五/六/七/八/十/十二节;任务 1-4 已提交代码按修订后架构搬迁,
  已提交 e2e 作搬迁回归网。
- **r1 补注(2026-11-06,用户综合复核)**:① 轮询设施明确定位为 VM 调度器
  的通用 IO readiness 设施(按 fd 通用,net 为第一个消费者,管道/进程 stdio
  与定时器线程统一为后续方向;regular file 不可 poll、走线程池,libuv/Go/Loom
  同此)——readiness 轮询抽象从 net 平台层拆出为 VM 侧 `IoPoller`,wepoll 仅
  编入 nlang_vm,模块不 poll;② 并发验证面取业内标准 echo 并发阶梯的代表性
  可比子集(ab/wrk 口径:并发→吞吐),不铺全压测工具面,便于跨语言对比。
  第三节补 libuv 双轨(DNS/文件走线程池)取证。
- **r1 补注 2(2026-11-06,实施计划阶段)**:ABI v11 增补第四回调
  `wakeIoWaiters(fd)`——close native 在 blocking 通道(无 GIL)需注销该 fd 的
  兴趣并唤醒停泊等待者,而模块不 poll、够不到 VM 轮询器,须经宿主回调
  (实施计划裁定 R13)。
- **r1 补注 3(2026-11-06,实施计划循环完善)**:① §五 `write`/`send`/
  `sendTo` 增默认参 `int count = -1`(实施计划裁定 R3:echo 只回发前 n
  字节的便利,视图实参由偏移感知助手解析);② §五开头补 `native` 标注
  语义说明(公开面实为脚本方法委托内部 native,D2/D9);③ D12/§六/§十.3
  链接面更正:VM 保留 ws2_32(wepoll 编入 VM、内部用 winsock 错误报告),
  仅去 iphlpapi;④ §六 `moduleState` 形态更正为 `(tag, ctor, dtor)`,ABI v11
  增补第五回调 `currentContextId`——按方向登记停泊等待者需要调用方上下文
  身份,同方向并发停泊拒绝(D11)据此判别(实施计划裁定 R14);⑤ §八
  blocking 通道 park 解卷/重入契约改由 e2e `stdlib_io_park_now`(纯截止停泊
  +停泊让出载体,单载体 `.flags`)加各 net 停泊用例钉死,不再列 ctest 项。
- **r1 补注 4(2026-11-06,用户整体架构裁定)**:工作线程池不随 net 模块
  私有,是通用并发基础设施——引擎归 VM(并发库的原生引擎层:载体/Channel/
  Mutex 引擎皆在 VM,模块只依赖 ABI、彼此不能共享代码),新增
  `src/vm/VmWorkPool`,经 ABI v11 第六回调 `submitWork(fn, arg, ackFd,
  priority)` 暴露给原生模块;net 的 DNS 解析/ICMP echo 卸载是第一个消费者
  (D5 据此修订),fs 常规文件 IO 将来复用,脚本面便利类(concurrent 包
  ThreadPool)随首个脚本级消费者再议;priority 两级队列在池有空闲 worker 时保住
  "慢解析不延迟 ping"裁定(libuv `uv_queue_work` 先例;满载时受排队影响,
  priority 不抢占在跑调用)。wepoll 许可措辞一并更正为
  BSD-2-Clause(piscisaureus/wepoll 实际许可,实施台账任务 1 裁决)。
  D4/D5/§六/§十.3 据此修订。
- **r1 补注 5(2026-11-06,用户整体架构盘点指令)**:线程池仅为示例,按语言/
  工具链/标准库整体架构盘点通用设施,concurrent 与 io 两包吸收网络库开发
  成果。结论:① `toBytes`/`fromBytes` 移入 `io` 包(通用 UTF-8 字节转换,
  io 已拥有字节容器类族,fs/process/HTTP 将来复用;net.n 脚本便捷面改用
  模块内部 `_toBytes`/`_fromBytes`,D7/D13/§五/§九/§十同步);② §六 增通用
  设施盘点表(停泊/工作线程池/字节转换/模块状态/自管道 ack/流分帧/定时器
  线程/跨包 import 的归属与提取点);③ concurrent 包脚本面 ThreadPool 维持
  "随首个脚本级消费者"裁定并补正面理由——脚本闭包已有虚拟线程
  (thread.spawn),工作线程池的价值在卸载原生阻塞调用,脚本面与虚拟线程
  语义重复;④ 跨包 import 记为语言/工具链路线图项(跨包基类与助手共享的
  前提,本项目范围外)。
