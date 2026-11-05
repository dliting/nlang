# 通用网络库设计(net 包)

- **文档定位**:nlang 通用网络库的立项级概要设计(Spec)。定义功能面、脚本层 API、
  native 分层、并发/停泊集成、错误模型、测试与验收;实施计划由 writing-plans 另文产出。
- **输入**:用户立项要求(高并发、TCP/UDP/ICMP、不向下兼容、将来 HTTP 构建其上、
  API 方便调用且符合现代网络库命名习惯、配套高并发示例);主流网络库联网调研
  (第三节,带出处);仓库基础设施勘察(标准库打包、intrinsic 通道、调度器停泊机制)。
- **裁定原则**:不向下兼容;选最贴合 nlang 虚拟线程模型的最合适方案;KISS,
  机制层复用既有调度器停泊先例,不为网络另造异步体系。
- **状态**:草案,待用户审阅。

## 一、目标与范围

**v1 做**:

1. `net` 标准库包:TCP(监听/连接/流式读写)、UDP(单播/广播/组播)、
   ICMP(ping 便利面 + IcmpSocket 报文面)。
2. 高并发:阻塞风格 API 跑在虚拟线程上,百万级停泊连接不受载体数限制
   (验收 1000 并发连接入 e2e,10000 入本地基准)。
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

1. **阻塞 native 调用(regBlocking)并发槽位 ≤ 载体数**(nvm 上限 4):
   每次在飞阻塞 native 占用一个载体 OS 线程,超出排队。因此 socket 阻塞操作
   **绝不能**走 regBlocking——1 万连接各停在一个 `recv` 上时只有 4 个能真正
   等在内核里。必须走 Channel/Mutex 同款路线:挂起虚拟线程 + 专用线程唤醒,
   停泊数不受载体数限制。
2. 带每实例 C++ 状态的句柄类只有 VM 内建 intrinsic 通道(通道 B)有先例
   (Thread/Mutex/Channel/Future);host dll 通道(通道 A)只验证过自由函数。
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
| JDK 21 虚拟线程(JEP 444):JDK 改造 `java.net.Socket` 内部为非阻塞+poller,虚拟线程阻塞在网络读上即从载体卸载;同步阻塞 API + M:N 调度即可达近异步吞吐——与 nlang"阻塞式等待即全部等待形态"完全同构的先例 | [JEP 444](https://openjdk.org/jeps/444)、[Java 21 虚拟线程机制综述(掘金,2026)](https://juejin.cn/post/7677771411977044003) |
| Windows 上 `SOCK_RAW/IPPROTO_ICMP` 自 XP SP2 起需管理员(否则 WSAEPERM/10013);非特权 ping 的正规路线是 IP Helper API `IcmpCreateFile`/`IcmpSendEcho2`(自动处理校验和与 ID/序列) | [Windows ICMP 权限与 IcmpSendEcho(php.cn,2026-02)](https://www.php.cn/faq/2117702.html)、[SO 讨论:ICMP raw 仅限管理员](https://stackoverflow.com/questions/745602) |
| Linux 非特权 ICMP:`SOCK_DGRAM/IPPROTO_ICMP` + `net.ipv4.ping_group_range`(内核 3.0+;Fedora/Ubuntu 现代发行版默认放开);内核自动填 ID 与校验和,无 IP 头 | [icmp(7) 转述(Stack Overflow)](https://stackoverflow.org.cn/questions/1189389)、[Fedora ping_group_range 变更说明](https://fedoraproject.org/wiki/Changes/EnableSysctlPingGroupRange) |
| Windows 多路复用:select 受 FD_SETSIZE 1024 限制,WSAPoll 有已知兼容性问题(非阻塞 connect 可写性误报等);wepoll 以 IOCP 实现 epoll 兼容 API(2 个文件,MIT,线程安全,支持数十万 fd),是 reactor 模型在 Windows 的成熟落点 | [wepoll 项目解析(CSDN,2025-11)](https://blog.csdn.net/gitblog_00754/article/details/155084876) |
| 脚本语言网络库 API 粒度先例:Node.js(net/dgram 连接对象+事件)、LuaSocket(连接对象+超时参数)、Go(net.Dial/Conn.Read 阻塞流)——主流都收敛到"连接对象 + 阻塞/等待方法 + 超时参数",不向脚本层暴露 fd/epoll | Node.js/LuaSocket/Go 官方文档 |

## 四、架构裁定

| # | 决策 | 要点与理由 |
| --- | --- | --- |
| D1 | **同步阻塞风格 API,无 async/await/回调** | 与 nlang 并发模型(阻塞式等待即全部等待形态,0.10.3 裁定)一致;Go/JDK 21 同构先例(第三节)。高并发来自"每连接一虚拟线程 + 停泊不占载体",不来自 API 染色 |
| D2 | **实现通道 = VM 内建 intrinsic + 句柄类(通道 B)** | 第二节事实 1/2:regBlocking 槽位封顶与高并发矛盾;句柄类先例只有通道 B。代价是 VM 内核增大,与"部分功能用 nlang 实现"的平衡由脚本层便利面(readLine/write(string)/ping 组装)兑付 |
| D3 | **非阻塞 socket + 专用 IO 轮询线程 + `ParkedIo` 停泊** | 复刻定时器线程模板(`ParkForIo(id, slot, interest, wakeAt)` 惰性启动);阻塞操作 = 尝试非阻塞 syscall → would-block 则登记兴趣并 `RequestPark` → 轮询线程收割就绪 → 唤醒重试。停泊上下文数只受内存限制 |
| D4 | **轮询后端:Windows 用 vendored wepoll;Linux epoll 预留** | 第二节 WSAPoll 兼容性陷阱;wepoll=IOCP 之上的 epoll 兼容层,与将来 Linux 后端同一套 reactor 语义,代码路径合一。src/3rdparty 已有 vendoring 先例(tinyxml2/libvterm),MIT 2 文件 |
| D5 | **DNS 走专用解析线程;ICMP echo 在 Windows 另用专用工作线程** | `getaddrinfo` 是阻塞调用,直接在载体上跑会破坏单载体确定性;`IcmpSendEcho2` 同步形态同理。两者各用一条惰性启动的专用线程(队列化,完成即唤醒停泊上下文),不共用——慢解析(可达 5s)不应延迟 ping;均不占 regBlocking 槽位。Linux 的 ICMP 走 socket+轮询线程,无需工作线程 |
| D6 | **ICMP 双路线**:ping 便利面按平台走非特权路线;IcmpSocket 报文面 Windows=raw(需管理员,错误信息明示)、Linux=dgram | 第三节权限结论。ping() 在 Windows 用 `IcmpSendEcho2`(免管理员),Linux 用 DGRAM ICMP socket;IcmpSocket 在 Windows raw 发送时由 native 自动补 ICMP 校验和 |
| D7 | **数据面 = `byte[]` 缓冲 + Go 式读写语义** | `read(buf)` 至多填 buf.length,返回字节数,0=EOF;`write(data)` 全写(内部循环),返回 data.length。string 经 `net.toBytes/fromBytes` 进出(不动冻结的 string 方法面) |
| D8 | **超时 = 每阻塞操作 `timeoutMs = 0`(0 为无限),到期抛 `TimeoutException`** | 超时截止与 IO 兴趣同时登记;轮询线程的等待上限取最近截止(复刻定时器线程"睡到最近到期"模式),到期与就绪先到先唤醒,唤醒后复查决定返回或抛。不耦合定时器线程,避免双线程竞态 |
| D9 | **异常 = `SocketException : IOException` 与 `TimeoutException : IOException`,声明在 net.n,native ctor** | ChannelClosedException 同款(VM 侧构造抛出);catch IOException 覆盖全部网络错误,catch TimeoutException 单独处理超时。DNS 失败/拒连/重置均 SocketException,消息带主机端口上下文 |
| D10 | **地址 = 字符串表示**("192.168.1.1"、"[::1]"、"example.com");`bind(port)` 默认 "0.0.0.0";端口 0 自动分配经 `localPort()` 读回 | 脚本层不引入 Endpoint 值类型(YAGNI;Go 的 string 地址面最省学习成本);解析发生在 connect/bind/ping 内部,透明 |
| D11 | **每 socket 每方向同时至多一个阻塞操作;close() 唤醒停泊者并使其抛 SocketException** | 与 Go"并发读写允许"不同,nlang 取确定性:第二个同方向停泊者直接 SocketException("concurrent operation"),避免未定义竞态;close 语义对齐 Channel close 的唤醒先例 |
| D12 | **资源生命周期 = 显式 `close()`(VM 无 finalizer);进程退出由 OS 回收** | 句柄类持 `__handle`(RTK_Int32)指向 VM 侧 socket 槽表;槽表条目持 fd+停泊上下文+兴趣掩码;close 释放槽并唤醒。Runtime 静态收尾统一关闭未关 socket 并 `WSACleanup` |

## 五、脚本层 API 面(net.n 权威声明草案)

```nlang
// net.n — 网络通信包。需显式 import net;

// ---- 异常(VM 侧构造抛出;脚本亦可主动 throw) ----
const class SocketException : IOException {
    public native SocketException(string message, Exception cause = null);
}
const class TimeoutException : IOException {
    public native TimeoutException(string message, Exception cause = null);
}

// ---- 字节助手(string 是字节容器;转换不动 string 方法面) ----
native byte[] toBytes(string s);
native string fromBytes(byte[] data);

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
    // 全写:内部循环直至 data 全部发出;返回 data.length。
    public native int write(byte[] data);
    // 读一行(以 '\n' 结尾,返回不含行尾的文本;'\r' 被剥离)。
    // 对端在行结束前关闭返回已积累文本;零字节且 EOF 返回 null。脚本层实现。
    public string readLine(int timeoutMs = 0);
    // 读满 count 字节(HTTP 定长体分帧);EOF 早至抛 SocketException。脚本层实现。
    public byte[] readExactly(int count, int timeoutMs = 0);
    // 文本便捷面(重载,§7.3):write(toBytes(s))。脚本层实现。
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
    public native int sendTo(byte[] data, string host, int port);
    public native int recvFrom(byte[] buf, out string peerAddress, out int peerPort,
                               int timeoutMs = 0);   // 数据报截断则丢弃多余字节(POSIX 语义)
    // 关联模式:connect 后可用 send/recv 与 peerAddress()/peerPort()。
    public native void connect(string host, int port);
    public native int send(byte[] data);
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
src/vm/IntrinsicsNetTcp.cpp        TCP 句柄内在函数(bind/accept/connect/read/write/选项)
src/vm/IntrinsicsNetUdp.cpp        UDP 句柄内在函数
src/vm/IntrinsicsNetIcmp.cpp       IcmpSocket + ping 内在函数
src/vm/IntrinsicsNetSupport.cpp    toBytes/fromBytes/异常 ctor/内在函数分派家族入口
src/vm/VmSchedulerIo.cpp           IO 轮询线程(ParkForIo/兴趣表/收割唤醒/最近截止睡眠)
src/vm/NetResolver.cpp             DNS 解析线程 + ICMP-echo 工作线程(阻塞 OS 调用卸载)
src/vm/net/NetPlatform.h           平台抽象:create/bind/listen/connect/recv/send/
                                   setsockopt/poll 注册/ICMP 原语
src/vm/net/NetPlatformWindows.cpp  winsock2 + vendored wepoll + IcmpSendEcho2
src/vm/net/NetPlatformPosix.cpp    POSIX 骨架(v1 编译通过即可,epoll 实装留移植任务)
src/3rdparty/wepoll/               vendored wepoll.c/wepoll.h(MIT,LICENSE 登记)
```

- **intrinsic 家族接入**:`Intrinsics.h` 追加 ID(自 173 起,约 44 个,只追加
  不复用);`BindingTable.cpp` 按 `pkg.name`/`Class.method`/`Class/N` 约定加行;
  `s_HandleClassNames` 加 TcpListener/TcpStream/UdpSocket/IcmpSocket;
  `VmExecutorIntrinsics.cpp` 级联加 net 家族分支。`no_builtin_stdlib` 守卫只禁
  旧 math/io/fs 表复活,新家族合法。
- **socket 槽表**:`NetSocketSlot` 注册表(仿 `SyncMutexSlotAt`):
  `{fd, kind, closed, readWaiterCtx, writeWaiterCtx, acceptWaiterCtx, interestMask}`;
  `__handle` 存槽 id;close 置 closed、注销兴趣、唤醒停泊者、释放槽
  (id 代际计数防 ABA,仿既有句柄表纪律)。
- **调度器扩展**:`ContextState::ParkedIo` 一个状态(读/写/接受方向记在槽上,
  不分裂状态族);`ParkForIo` 复刻 `ParkForTimer` 的惰性启动与两阶段 park 握手
  (`framesParked`,防丢唤醒);唤醒走调度器既有 Wake 路径。
- **轮询线程主循环**:wepoll_wait(超时 = 兴趣表内最近截止,无兴趣则长睡)
  → 收割事件 → 取 GIL(`GilEnsureGuard`)→ 对每个就绪槽唤醒对应停泊上下文
  (置 Runnable);对每个已过期截止唤醒并标记超时。close/新兴趣登记都 notify
  轮询线程重算等待上限(复刻定时器线程 cv 模式)。
- **GIL 纪律**:非阻塞 syscall(send/recv 尝试)在 intrinsic 内持 GIL 执行
  (µs 级,不阻塞回收会合);轮询等待、getaddrinfo、IcmpSendEcho2 在无 GIL 的
  专用线程上跑。停泊/唤醒全程走 GIL 下状态迁移。
- **GC 交互**:停泊上下文的帧(含 byte[] 缓冲引用)由既有跨挂起根集覆盖
  (精确扫+保守带);intrinsic 新铸句柄沿用 RootMinted 扎根纪律;无新增根面。
- **WSA 生命周期**:`Runtime::StaticInit` 内 `WSAStartup`(仅 VM 执行路径,
  编译器 ncc 不受影响),进程级一次;socket 槽表**归属 VmExecutor 实例**
  (与 VM 其余状态同域,进程内多执行器互不串扰),执行器收尾统一关闭未关
  socket;`WSACleanup` 在宿主退出路径执行。
- **GIL 下 syscall**:非阻塞 syscall 在 GIL 内执行意味着不同 socket 的操作跨载体
  串行;µs 级 syscall 对 v1 目标(1000 停泊连接)绰绰有余,"syscall 挪出 GIL"
  记为将来性能线,不入 v1。
- **链接**:VM 库追加 `ws2_32` + `iphlpapi`(Windows)。
- **source_size_guard**:500 行/文件、50 行/函数——家族按上表拆分 TU 预防。

## 七、关键数据流

**阻塞 read(无超时)**:
1. intrinsic 持 GIL:查槽(closed→抛 SocketException;同方向已有停泊者→抛)→
   非阻塞 `recv` → 有数据直接返回拷贝进 buf。
2. `WSAEWOULDBLOCK` → 登记 readWaiterCtx + 兴趣 EPOLLIN → `ParkForIo`
   (两阶段握手)→ 载体转去跑别的虚拟线程。
3. 轮询线程收割 EPOLLIN → 取 GIL → 唤醒该上下文 → 重试 recv(此时必不
   would-block)→ 返回。EPOLLHUP/ERR → 唤醒后按 EOF/错误处理。

**connect(host, port, timeoutMs)**:解析(停泊于解析线程)→ 逐地址非阻塞
connect,EINPROGRESS → 登记可写兴趣停泊 → 唤醒后 `getsockopt(SO_ERROR)`
判定 → 成功返回;失败尝试下一地址;全部失败或截止已到 → 抛。timeoutMs 预算
跨解析与连接两阶段(单调时钟截止点)。

**超时竞态**:截止点与兴趣同时登记;轮询线程睡到最近截止;到期唤醒与就绪唤醒
谁先到都行,唤醒后 intrinsic 复查"有数据?→返回;无数据且已过截止?→抛
TimeoutException"。复查以单调时钟为准,无丢唤醒(两阶段握手保证 park 完成前
的唤醒不丢)。

**close 竞态**:另一上下文 close → 槽置 closed → 注销兴趣 → 唤醒停泊者 →
停泊者复查 closed → 抛 SocketException("socket closed")。fd 在唤醒完成前
不真正 close(引用计数:停泊者退出后才关),防 fd 复用串扰。

## 八、测试策略

- **e2e(hermetic 回环,命名 `stdlib_net_*.n` 入 manifest.txt)**:
  tcp echo(服务器 spawn + 客户端收发 + join)、udp echo + recvFrom 来源断言、
  readLine/readExactly 分帧、toBytes/fromBytes 往返、connect 拒连抛
  SocketException、read 超时抛 TimeoutException(服务器收而不发)、
  connect("localhost") DNS 回环、close 唤醒停泊 reader、并发同方向拒绝、
  ping("127.0.0.1") 成功路径、IcmpSocket 非管理员 Windows 错误路径断言、
  组播 join 冒烟(239.x 回环)、千并发 echo(200 连接入 e2e;1000 入示例)。
  多载体变体加 `.flags --carriers=4`;GC 竞态按 gc-stress 纪律双形态跑
  (停泊 recv + 分配风暴)。
- **ctest(C++)**:socket 槽表生命周期(id 代际/close 唤醒/ABA)、
  轮询线程启停与兴趣登记竞态、wepoll 集成冒烟;进程内宿主先
  `Runtime::StaticInit`(既有纪律)。
- **符号索引测试**:函数计数 +183→新值,命名空间列表插入 net(字母序)。
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
单载体下也必须完成(停泊唤醒不依赖多载体)。

## 十、集成点与文档同步

1. `stdlib/net.n`(权威声明;GLOB 自动打包)+ `examples/*.n` 五例。
2. VM:Intrinsics.h(ID≥173)、BindingTable、s_HandleClassNames、
   VmExecutorIntrinsics 级联、新家族 TU 六个 + net/ 平台层三个 + VmSchedulerIo
   + NetResolver;CMake 链 ws2_32/iphlpapi;3rdparty wepoll 登记。
3. 测试:tests/CMakeLists.txt(新 ctest 项)、e2e manifest 条目、
   test_symbol_index.cpp 计数与列表。
4. 手册:library/11-conventions.md"六个包"→七个;新 20-net 页(en+zh);
   mkdocs.{en,zh}.yml nav;specification/index.md;19-library-exceptions.md
   增补两个异常类;docs/dev 本文档。
5. `CHANGELOG.md`/`CHANGELOG.zh-CN.md`;VERSION 0.12.6 → **0.13.0**(新功能次版本)。
6. 用户可见面同步:`docs/user_manual/{en,zh}/specification` 对应 spec 页
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
| wepoll vendoring(许可/构建集成/与 MSVC /W4 告警) | MIT 兼容;vendored 目标独立编译选项;若受阻退路 = WSAPoll 后端(接受已知怪癖)或直写 IOCP |
| Windows 防火墙对监听 socket 弹窗 | 测试/示例一律绑 127.0.0.1(回环监听不触发防火墙提示);文档建议服务器明示绑址 |
| e2e 端口冲突 | 一律端口 0 + localPort() |
| IcmpSocket 在 Windows 非管理员不可用 | 明示错误信息;ping() 覆盖免特权主场景;e2e 断言错误路径 |
| 千并发 e2e 在慢机上超时 | e2e 入 200 连接,1000 在示例门(30s 超时余量实测后钉死) |

**自主裁定(用户离场,按"请自主决策"授权,理由如下)**:① 包名取 `net`——
Go/Rust 现代惯例,短且涵盖 TCP/UDP/ICMP 全面;② IcmpSocket 进 v1——四个内在
函数成本低,且"支持 ICMP 协议"的立项要求不止于 ping;③ `TimeoutException`
挂 `IOException` 下——网络超时是 I/O 超时,`catch IOException` 应覆盖之,
与 Java `SocketTimeoutException` 谱系同理,将来非网络超时若需要再议。
